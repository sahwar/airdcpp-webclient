/*
 * Copyright (C) 2011-2012 AirDC++ Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#include "stdinc.h"

#include <boost/range/algorithm/for_each.hpp>
#include <boost/range/algorithm_ext/for_each.hpp>
#include <boost/random/discrete_distribution.hpp>
#include <boost/random/mersenne_twister.hpp>
#include <boost/fusion/include/count_if.hpp>
#include <boost/range/adaptor/map.hpp>

#include "BundleQueue.h"
#include "SettingsManager.h"
#include "AirUtil.h"
#include "QueueItem.h"
#include "LogManager.h"
#include "TimerManager.h"

namespace dcpp {

using boost::range::for_each;
using boost::adaptors::map_values;

BundleQueue::BundleQueue() : 
	nextSearch(0),
	nextRecentSearch(0)
{ }

BundleQueue::~BundleQueue() { }

void BundleQueue::addBundle(BundlePtr aBundle) {
	aBundle->unsetFlag(Bundle::FLAG_NEW);
	aBundle->setDownloadedBytes(0); //sets to downloaded segments

	addSearchPrio(aBundle);
	bundles[aBundle->getToken()] = aBundle;

	//check if we need to insert the root bundle dir
	if (!aBundle->isFileBundle()) {
		if (findLocalDir(aBundle->getTarget()) == bundleDirs.end()) {
			addDirectory(aBundle->getTarget(), aBundle);
		}
	}
}

void BundleQueue::addSearchPrio(BundlePtr aBundle) {
	if (aBundle->getPriority() < Bundle::LOW) {
		return;
	}

	if (aBundle->isRecent()) {
		dcassert(std::find(recentSearchQueue.begin(), recentSearchQueue.end(), aBundle) == recentSearchQueue.end());
		recentSearchQueue.push_back(aBundle);
		return;
	} else {
		dcassert(std::find(prioSearchQueue[aBundle->getPriority()].begin(), prioSearchQueue[aBundle->getPriority()].end(), aBundle) == prioSearchQueue[aBundle->getPriority()].end());
		prioSearchQueue[aBundle->getPriority()].push_back(aBundle);
	}
}

void BundleQueue::removeSearchPrio(BundlePtr aBundle) {
	if (aBundle->getPriority() < Bundle::LOW) {
		return;
	}

	if (aBundle->isRecent()) {
		auto i = std::find(recentSearchQueue.begin(), recentSearchQueue.end(), aBundle);
		//dcassert(i != recentSearchQueue.end());
		if (i != recentSearchQueue.end()) {
			recentSearchQueue.erase(i);
		}
	} else {
		auto i = std::find(prioSearchQueue[aBundle->getPriority()].begin(), prioSearchQueue[aBundle->getPriority()].end(), aBundle);
		//dcassert(i != prioSearchQueue[aBundle->getPriority()].end());
		if (i != prioSearchQueue[aBundle->getPriority()].end()) {
			prioSearchQueue[aBundle->getPriority()].erase(i);
		}
	}
}

BundlePtr BundleQueue::findSearchBundle(uint64_t aTick, bool force /* =false */) {
	BundlePtr bundle = nullptr;
	if(aTick >= nextSearch || force) {
		bundle = findAutoSearch();
		//LogManager::getInstance()->message("Next search in " + Util::toString(next) + " minutes");
	}
	
	if(!bundle && (aTick >= nextRecentSearch || force)) {
		bundle = findRecent();
		//LogManager::getInstance()->message("Next recent search in " + Util::toString(recentBundles > 1 ? 5 : 10) + " minutes");
	}
	return bundle;
}

int64_t BundleQueue::recalculateSearchTimes(bool aRecent, bool isPrioChange) {
	if (!aRecent) {
		int prioBundles = getPrioSum();
		int minInterval = SETTING(SEARCH_TIME);

		if (prioBundles > 0) {
			minInterval = max(60 / prioBundles, SETTING(SEARCH_TIME));
		}

		if (nextSearch > 0 && isPrioChange) {
			nextSearch = min(nextSearch, GET_TICK() + (minInterval * 60 * 1000));
		} else {
			nextSearch = GET_TICK() + (minInterval * 60 * 1000);
		}
		return nextSearch;
	} else {
		if (nextRecentSearch > 0 && isPrioChange) {
			nextRecentSearch = min(nextRecentSearch, GET_TICK() + getRecentIntervalMs());
		} else {
			nextRecentSearch = GET_TICK() + getRecentIntervalMs();
		}
		return nextRecentSearch;
	}
}

int BundleQueue::getRecentIntervalMs() const {
	int recentBundles = count_if(recentSearchQueue.begin(), recentSearchQueue.end(), [](BundlePtr b) { return b->allowAutoSearch(); });
	if (recentBundles == 1) {
		return 15 * 60 * 1000;
	} else if (recentBundles == 2) {
		return 8 * 60 * 1000;
	} else {
		return 5 * 60 * 1000;
	}
}

BundlePtr BundleQueue::findRecent() {
	if (recentSearchQueue.size() == 0) {
		return nullptr;
	}

	uint32_t count = 0;
	for (;;) {
		BundlePtr b = recentSearchQueue.front();
		recentSearchQueue.pop_front();

		//check if the bundle still belongs to here
		if (b->checkRecent()) {
			recentSearchQueue.push_back(b);
		} else {
			addSearchPrio(b);
		}

		if (b->allowAutoSearch()) {
			return b;
		} else if (count >= recentSearchQueue.size()) {
			break;
		}

		count++;
	}

	return nullptr;
}

boost::mt19937 gen;
static vector<double> probabilities;

int BundleQueue::getPrioSum() const {
	probabilities.clear();

	int prioBundles = 0;
	int p = Bundle::LOW;
	do {
		int dequeBundles = count_if(prioSearchQueue[p].begin(), prioSearchQueue[p].end(), [](BundlePtr b) { return b->allowAutoSearch(); });
		probabilities.push_back((p-1)*dequeBundles); //multiply with a priority factor to get bigger probability for higher prio bundles
		prioBundles += dequeBundles;
		p++;
	} while(p < Bundle::LAST);

	probabilities.shrink_to_fit();
	return prioBundles;
}

BundlePtr BundleQueue::findAutoSearch() {
	int prioBundles = getPrioSum();

	//do we have anything where to search from?
	if (prioBundles == 0) {
		return nullptr;
	}

	auto dist = boost::random::discrete_distribution<>(probabilities);

	//choose the search queue, can't be paused or lowest
	auto& sbq = prioSearchQueue[dist(gen) + 2];
	dcassert(!sbq.empty());

	//find the first item from the search queue that can be searched for
	auto s = find_if(sbq.begin(), sbq.end(), [](BundlePtr b) { return b->allowAutoSearch(); } );

	if (s != sbq.end()) {
		BundlePtr b = *s;
		//move to the back
		sbq.erase(s);
		sbq.push_back(b);
		return b;
	}

	return nullptr;
}

BundlePtr BundleQueue::findBundle(const string& bundleToken) const {
	auto i = bundles.find(bundleToken);
	if (i != bundles.end()) {
		return i->second;
	}
	return nullptr;
}

pair<string, BundlePtr> BundleQueue::findRemoteDir(const string& aDir) const {
	if (aDir.size() < 3)
		return make_pair(Util::emptyString, nullptr);

	//get the last directory, we might need the position later with subdirs
	string remoteDir = aDir;
	if (remoteDir[remoteDir.length()-1] == PATH_SEPARATOR)
		remoteDir.erase(aDir.size()-1, aDir.size());
	auto pos = remoteDir.rfind(PATH_SEPARATOR);
	if (pos != string::npos)
		remoteDir = remoteDir.substr(pos+1);

	auto directories = bundleDirs.equal_range(remoteDir);
	if (directories.first == directories.second)
		return make_pair(Util::emptyString, nullptr);

	//check the parents for dirs like CD1 to prevent false matches
	if (boost::regex_match(remoteDir, AirUtil::subDirRegPlain) && pos != string::npos) {
		string::size_type i, j;
		remoteDir = PATH_SEPARATOR + aDir;

		for(auto s = directories.first; s != directories.second; ++s) {
			//start matching from the parent dir, as we know the last one already
			i = pos;
			string curDir = s->second.first;
			bool found = false;

			for(;;) {
				j = remoteDir.find_last_of("\\", i);
				if(j == string::npos || (int)(curDir.length() - (remoteDir.length() - j)) < 0) //also check if it goes out of scope for the local dir
					break;
				if(stricmp(remoteDir.substr(j+1, i-j), curDir.substr(curDir.length() - (remoteDir.length()-j)+1, i-j)) == 0) {
					if (!boost::regex_match(remoteDir.substr(j+1, i-j), AirUtil::subDirRegPlain)) { //another subdir? don't break in that case
						found = true;
						break;
					}
				} else {
					//this is something different... continue to next match
					break;
				}
				i = j - 1;
			}

			if (found) {
				return s->second;
			}
		}

		return make_pair(Util::emptyString, nullptr);
	}

	return directories.first->second;
}

void BundleQueue::getInfo(const string& aSource, BundleList& retBundles, int& finishedFiles, int& fileBundles) const {
	BundlePtr tmpBundle;
	bool subFolder = false;

	//find the matching bundles
	for(auto j = bundles.cbegin(), iend = bundles.cend(); j != iend; ++j) {
		tmpBundle = j->second;
		if (tmpBundle->isFinished()) {
			//don't modify those
			continue;
		}

		if (AirUtil::isParentOrExact(aSource, tmpBundle->getTarget())) {
			//parent or the same dir
			retBundles.push_back(tmpBundle);
			if (tmpBundle->isFileBundle())
				fileBundles++;
		} else if (!tmpBundle->isFileBundle() && AirUtil::isSub(aSource, tmpBundle->getTarget())) {
			//subfolder
			retBundles.push_back(tmpBundle);
			subFolder = true;
			break;
		}
	}

	//count the finished files
	if (subFolder) {
		for_each(tmpBundle->getFinishedFiles(), [&](QueueItemPtr qi) { 
			if(AirUtil::isSub(qi->getTarget(), aSource)) 
				finishedFiles++; 
		});
	} else {
		for_each(retBundles, [&](BundlePtr b) { finishedFiles += b->getFinishedFiles().size(); });
	}
}

BundlePtr BundleQueue::getMergeBundle(const string& aTarget) const {
	/* Returns directory bundles that are in sub or parent dirs (or in the same location), in which we can merge to */
	for(auto i = bundles.cbegin(), iend = bundles.cend(); i != iend; ++i) {
		BundlePtr compareBundle = i->second;
		if (!compareBundle->isFileBundle() && (AirUtil::isSub(aTarget, compareBundle->getTarget()) || AirUtil::isParentOrExact(aTarget, compareBundle->getTarget()))) {
			return compareBundle;
		}
	}
	return nullptr;
}

void BundleQueue::getSubBundles(const string& aTarget, BundleList& retBundles) const {
	/* Returns bundles that are inside aTarget */
	for_each(bundles | map_values, [&](BundlePtr compareBundle) {
		if (AirUtil::isSub(compareBundle->getTarget(), aTarget)) {
			retBundles.push_back(compareBundle);
		}
	});
}

void BundleQueue::addBundleItem(QueueItemPtr qi, BundlePtr aBundle) {
	if (aBundle->addQueue(qi) && !aBundle->isFileBundle()) {
		addDirectory(qi->getFilePath(), aBundle);
	}
}

void BundleQueue::removeBundleItem(QueueItemPtr qi, bool finished) {
	if (qi->getBundle()->removeQueue(qi, finished) && !finished && !qi->getBundle()->isFileBundle()) {
		removeDirectory(qi->getFilePath());
	}
}

void BundleQueue::addDirectory(const string& aPath, BundlePtr aBundle) {
	bundleDirs.insert(make_pair(Util::getLastDir(aPath), make_pair(aPath, aBundle)));
}

void BundleQueue::removeDirectory(const string& aPath) {
	auto p = findLocalDir(aPath);
	if (p != bundleDirs.end()) {
		bundleDirs.erase(p);
	}
}

Bundle::BundleDirMap::iterator BundleQueue::findLocalDir(const string& aPath) {
	auto bdr = bundleDirs.equal_range(Util::getLastDir(aPath));
	for(auto dp = bdr.first; dp != bdr.second; ++dp) {
		if (dp->second.first == aPath) {
			return dp;
		}
	}
	return bundleDirs.end();
}

void BundleQueue::addFinishedItem(QueueItemPtr qi, BundlePtr aBundle) {
	if (aBundle->addFinishedItem(qi, false) && !aBundle->isFileBundle()) {
		addDirectory(qi->getFilePath(), aBundle);
	}
}

void BundleQueue::removeFinishedItem(QueueItemPtr qi) {
	if (qi->getBundle()->removeFinishedItem(qi) && !qi->getBundle()->isFileBundle()) {
		removeDirectory(qi->getFilePath());
	}
}

void BundleQueue::removeBundle(BundlePtr aBundle) {
	if (aBundle->isSet(Bundle::FLAG_NEW)) {
		return;
	}

	for_each(aBundle->getBundleDirs(), [&](pair<string, pair<uint32_t, uint32_t>> d) {
		removeDirectory(d.first);
	});

	//make sure that everything will be freed from the memory
	dcassert(aBundle->getFinishedFiles().empty());
	dcassert(aBundle->getQueueItems().empty());

	/*for(auto i = aBundle->getFinishedFiles().begin(); i != aBundle->getFinishedFiles().end(); )
		aBundle->getFinishedFiles().erase(i);
	for(auto i = aBundle->getQueueItems().begin(); i != aBundle->getQueueItems().end(); )
		aBundle->getQueueItems().erase(i);*/

	removeSearchPrio(aBundle);
	bundles.erase(aBundle->getToken());

	aBundle->deleteBundleFile();
}

void BundleQueue::moveBundle(BundlePtr aBundle, const string& newTarget) {
	//remove the old release dir
	removeDirectory(aBundle->getTarget());

	aBundle->setTarget(newTarget);

	//add new
	addDirectory(newTarget, aBundle);
}

void BundleQueue::getDiskInfo(TargetUtil::TargetInfoMap& dirMap, const StringSet& volumes) const {
	string tempVol;
	bool useSingleTempDir = (SETTING(TEMP_DOWNLOAD_DIRECTORY).find("%[targetdrive]") == string::npos);
	if (useSingleTempDir) {
		tempVol = TargetUtil::getMountPath(SETTING(TEMP_DOWNLOAD_DIRECTORY), volumes);
	}

	for_each(bundles | map_values, [&](BundlePtr b) {
		string mountPath = TargetUtil::getMountPath(b->getTarget(), volumes);
		if (!mountPath.empty()) {
			auto s = dirMap.find(mountPath);
			if (s != dirMap.end()) {
				bool countAll = (useSingleTempDir && (mountPath != tempVol));
				s->second.queued += b->getDiskUse(countAll);
			}
		}
	});
}

void BundleQueue::saveQueue(bool force) noexcept {
	try {
		for_each(bundles | map_values, [force](BundlePtr b) {
			if (!b->isFinished() && (b->getDirty() || force)) 
				b->save();
		});
	} catch(...) {
		// ...
	}
}

} //dcpp