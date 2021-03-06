/*
* Copyright (C) 2001-2019 Jacek Sieka, arnetheduck on gmail point com
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

#ifndef DCPLUSPLUS_DCPP_SHAREPATH_VALIDATOR_H
#define DCPLUSPLUS_DCPP_SHAREPATH_VALIDATOR_H

#include "ActionHook.h"
#include "CriticalSection.h"
#include "File.h"
#include "StringMatch.h"

#include "typedefs.h"

namespace dcpp {

class SharePathValidator {
public:
	ActionHook<const string&, int64_t> fileValidationHook;
	ActionHook<const string&> directoryValidationHook;

	SharePathValidator();

	// Get a list of excluded real paths
	StringSet getExcludedPaths() const noexcept;
	void setExcludedPaths(const StringSet& aPaths) noexcept;

	// Add an excluded path
	// Throws ShareException if validation fails
	void addExcludedPath(const string& aPath);
	bool removeExcludedPath(const string& aPath) noexcept;

	// Prepares the skiplist regex after the pattern has been changed
	void reloadSkiplist();

	// Check if a directory/file name matches skiplist
	bool matchSkipList(const string& aName) const noexcept;

	void validate(FileFindIter& aIter, const string& aPath, bool aSkipQueueCheck) const;

	void saveExcludes(SimpleXML& xml) const noexcept;
	void loadExcludes(SimpleXML& xml) noexcept;

	// Check that the root path is valid to be added in share
	// Use checkSharedName for non-root directories
	void validateRootPath(const string& aRealPath) const;

	// Check the list of directory path tokens relative to the base path
	// Returns whether they are all valid to be added in share
	void validatePathTokens(const string& aBasePath, const StringList& aTokens, bool aSkipQueueCheck) const;
private:
	// Comprehensive check for a directory/file whether it is valid to be added in share
	// Use validateRootPath for new root directories instead
	void checkSharedName(const string& aPath, bool aIsDirectory, int64_t aSize = 0) const;

	bool isExcluded(const string& aPath) const noexcept;

	StringMatch skipList;
	string winDir;

	// Excluded paths with exact casing
	// Use refreshMatcherCS for locking
	StringSet excludedPaths;

	mutable SharedMutex cs;
};


} // namespace dcpp

#endif // !defined(SHARE_MANAGER_H)
