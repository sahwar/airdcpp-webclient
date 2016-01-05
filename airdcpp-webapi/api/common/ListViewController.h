/*
* Copyright (C) 2011-2015 AirDC++ Project
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

#ifndef DCPLUSPLUS_DCPP_LISTVIEW_H
#define DCPLUSPLUS_DCPP_LISTVIEW_H

#include <web-server/stdinc.h>
#include <web-server/JsonUtil.h>
#include <web-server/SessionListener.h>
#include <web-server/WebServerManager.h>

#include <airdcpp/TaskQueue.h>
#include <airdcpp/TimerManager.h>

#include <api/ApiModule.h>
#include <api/common/PropertyFilter.h>
#include <api/common/Serializer.h>

namespace webserver {

	template<class T, int PropertyCount>
	class ListViewController : private SessionListener {
	public:
		typedef typename PropertyItemHandler<T>::ItemList ItemList;
		typedef typename PropertyItemHandler<T>::ItemListFunction ItemListF;
		typedef std::function<void(bool aActive)> StateChangeFunction;

		// Use the short default update interval for lists that can be edited by the users
		// Larger lists with lots of updates and non-critical response times should specify a longer interval
		ListViewController(const string& aViewName, ApiModule* aModule, const PropertyItemHandler<T>& aItemHandler, ItemListF aItemListF, time_t aUpdateInterval = 200) :
			module(aModule), viewName(aViewName), itemHandler(aItemHandler), itemListF(aItemListF),
			timer(WebServerManager::getInstance()->addTimer([this] { runTasks(); }, aUpdateInterval))
		{
			aModule->getSession()->addListener(this);

			// Magic for the following defines
			auto& requestHandlers = aModule->getRequestHandlers();

			auto access = aModule->getSubscriptionAccess();
			METHOD_HANDLER(viewName, access, ApiRequest::METHOD_POST, (EXACT_PARAM("filter")), false, ListViewController::handlePostFilter);
			METHOD_HANDLER(viewName, access, ApiRequest::METHOD_PUT, (EXACT_PARAM("filter"), TOKEN_PARAM), true, ListViewController::handlePutFilter);
			METHOD_HANDLER(viewName, access, ApiRequest::METHOD_DELETE, (EXACT_PARAM("filter"), TOKEN_PARAM), false, ListViewController::handleDeleteFilter);

			METHOD_HANDLER(viewName, access, ApiRequest::METHOD_POST, (EXACT_PARAM("settings")), true, ListViewController::handlePostSettings);
			METHOD_HANDLER(viewName, access, ApiRequest::METHOD_DELETE, (), false, ListViewController::handleReset);

			METHOD_HANDLER(viewName, access, ApiRequest::METHOD_GET, (EXACT_PARAM("items"), NUM_PARAM, NUM_PARAM), false, ListViewController::handleGetItems);
		}

		~ListViewController() {
			module->getSession()->removeListener(this);

			timer->stop(true);
		}

		void setActiveStateChangeHandler(StateChangeFunction aF) {
			stateChangeF = aF;
		}

		void stop() noexcept {
			setActive(false);
			timer->stop(false);

			clear();
			currentValues.reset();
		}

		void resetItems() {
			clear();

			currentValues.set(IntCollector::TYPE_RANGE_START, 0);

			updateList();
		}

		void onItemAdded(const T& aItem) {
			if (!active) return;

			tasks.addItem(aItem);
		}

		void onItemRemoved(const T& aItem) {
			if (!active) return;

			tasks.removeItem(aItem);
		}

		void onItemUpdated(const T& aItem, const PropertyIdSet& aUpdatedProperties) {
			if (!active) return;

			tasks.updateItem(aItem, aUpdatedProperties);
		}

		void onItemsUpdated(const ItemList& aItems, const PropertyIdSet& aUpdatedProperties) {
			if (!active) return;

			for (const auto& item : aItems) {
				onItemUpdated(item, aUpdatedProperties);
			}
		}

		void clearFilters() {
			{
				WLock l(cs);
				filters.clear();
			}

			onFilterUpdated();
		}

		bool isActive() const noexcept {
			return active;
		}
	private:
		void setActive(bool aActive) {
			active = aActive;
			if (stateChangeF) {
				stateChangeF(aActive);
			}
		}

		// FILTERS START
		PropertyFilter::Matcher::List getFilterMatchers() {
			PropertyFilter::Matcher::List ret;

			RLock l(cs);
			for (auto& filter : filters) {
				if (!filter->empty()) {
					ret.emplace_back(filter);
				}
			}

			return ret;
		}

		PropertyFilter::List::iterator findFilter(FilterToken aToken) {
			return find_if(filters.begin(), filters.end(), [&](const PropertyFilter::Ptr& aFilter) { return aFilter->getId() == aToken; });
		}

		bool removeFilter(FilterToken aToken) {
			{
				WLock l(cs);

				auto filter = findFilter(aToken);
				if (filter == filters.end()) {
					return false;
				}

				filters.erase(filter);
			}

			onFilterUpdated();
			return true;
		}

		PropertyFilter::Ptr addFilter() {
			auto filter = make_shared<PropertyFilter>(itemHandler.properties);

			{
				WLock l(cs);
				filters.push_back(filter);
			}

			return filter;
		}

		bool matchesFilter(const T& aItem, const PropertyFilter::Matcher::List& aMatchers) {
			return PropertyFilter::Matcher::match(aMatchers,
				[&](size_t aProperty) { return itemHandler.numberF(aItem, aProperty); },
				[&](size_t aProperty) { return itemHandler.stringF(aItem, aProperty); },
				[&](size_t aProperty, const StringMatch& aStringMatcher, double aNumericMatcher) { return itemHandler.customFilterF(aItem, aProperty, aStringMatcher, aNumericMatcher); }
			);
		}

		void setFilterProperties(const json& aRequestJson, PropertyFilter::Ptr& aFilter) {
			auto method = JsonUtil::getField<int>("method", aRequestJson);
			auto property = JsonUtil::getField<string>("property", aRequestJson);

			// Pattern can be string or numeric
			string pattern;
			auto patternJson = JsonUtil::getRawValue("pattern", aRequestJson);
			if (patternJson.is_number()) {
				pattern = Util::toString(JsonUtil::parseValue<double>("pattern", patternJson));
			} else {
				pattern = JsonUtil::parseValue<string>("pattern", patternJson);
			}

			aFilter->prepare(pattern, method, findPropertyByName(property, itemHandler.properties));
			onFilterUpdated();
		}

		api_return handlePostFilter(ApiRequest& aRequest) {
			const auto& reqJson = aRequest.getRequestBody();

			auto filter = addFilter();
			if (!reqJson.is_null()) {
				setFilterProperties(reqJson, filter);
			}

			aRequest.setResponseBody({ 
				{ "id", filter->getId() }
			});
			return websocketpp::http::status_code::ok;
		}

		api_return handlePutFilter(ApiRequest& aRequest) {
			const auto& reqJson = aRequest.getRequestBody();
			PropertyFilter::Ptr filter = nullptr;

			{
				WLock l(cs);
				auto i = findFilter(aRequest.getTokenParam(1));
				if (i == filters.end()) {
					aRequest.setResponseErrorStr("Filter not found");
					return websocketpp::http::status_code::bad_request;
				}

				filter = *i;
			}

			setFilterProperties(reqJson, filter);
			return websocketpp::http::status_code::no_content;
		}

		api_return handleDeleteFilter(ApiRequest& aRequest) {
			const auto& reqJson = aRequest.getRequestBody();

			if (!removeFilter(aRequest.getTokenParam(1))) {
				aRequest.setResponseErrorStr("Filter not found");
				return websocketpp::http::status_code::bad_request;
			}

			return websocketpp::http::status_code::no_content;
		}

		void onFilterUpdated() {
			ItemList itemsNew;
			auto matchers = getFilterMatchers();
			{
				RLock l(cs);
				for (const auto& i : allItems) {
					if (matchesFilter(i, matchers)) {
						itemsNew.push_back(i);
					}
				}
			}

			{
				WLock l(cs);
				matchingItems.swap(itemsNew);
				itemListChanged = true;
				currentValues.set(IntCollector::TYPE_RANGE_START, 0);
			}
		}

		// FILTERS END


		api_return handlePostSettings(ApiRequest& aRequest) {
			parseProperties(aRequest.getRequestBody());
			if (!active) {
				setActive(true);
				updateList();
				timer->start();
			}

			return websocketpp::http::status_code::no_content;
		}

		api_return handleReset(ApiRequest& aRequest) {
			if (!active) {
				aRequest.setResponseErrorStr("The view isn't active");
				websocketpp::http::status_code::bad_request;
			}

			stop();
			return websocketpp::http::status_code::no_content;
		}

		void parseProperties(const json& j) {
			typename IntCollector::ValueMap updatedValues;
			if (j.find("range_start") != j.end()) {
				int start = j["range_start"];
				if (start < 0) {
					throw std::invalid_argument("Negative range start not allowed");
				}

				updatedValues[IntCollector::TYPE_RANGE_START] = start;
			}

			if (j.find("max_count") != j.end()) {
				int end = j["max_count"];
				updatedValues[IntCollector::TYPE_MAX_COUNT] = end;
			}

			if (j.find("sort_property") != j.end()) {
				auto prop = findPropertyByName(j["sort_property"], itemHandler.properties);
				if (prop == -1) {
					throw std::invalid_argument("Invalid sort property");
				}

				updatedValues[IntCollector::TYPE_SORT_PROPERTY] = prop;
			}

			if (j.find("sort_ascending") != j.end()) {
				bool sortAscending = j["sort_ascending"];
				updatedValues[IntCollector::TYPE_SORT_ASCENDING] = sortAscending;
			}

			if (j.find("paused") != j.end()) {
				bool paused = j["paused"];
				if (paused && timer->isRunning()) {
					timer->stop(false);
				}
				else if (!paused && !timer->isRunning()) {
					timer->start();
				}
			}

			if (!updatedValues.empty()) {
				WLock l(cs);
				currentValues.set(updatedValues);
			}
		}

		void on(SessionListener::SocketDisconnected) noexcept {
			stop();
		}

		void sendJson(const json& j) {
			if (j.is_null()) {
				return;
			}

			module->send(viewName + "_updated", j);
		}

		int updateList() {
			WLock l(cs);
			matchingItems = itemListF();
			allItems.insert(matchingItems.begin(), matchingItems.end());
			itemListChanged = true;
			return static_cast<int>(matchingItems.size());
		}

		void clear() {
			WLock l(cs);
			tasks.clear();
			currentViewItems.clear();
			matchingItems.clear();
			allItems.clear();
			prevTotalItemCount = -1;
			prevMatchingItemCount = -1;
			filters.clear();
		}

		static bool itemSort(const T& t1, const T& t2, const PropertyItemHandler<T>& aItemHandler, int aSortProperty, int aSortAscending) {
			int res = 0;
			switch (aItemHandler.properties[aSortProperty].sortMethod) {
			case SORT_NUMERIC: {
				res = compare(aItemHandler.numberF(t1, aSortProperty), aItemHandler.numberF(t2, aSortProperty));
				break;
			}
			case SORT_TEXT: {
				res = Util::stricmp(aItemHandler.stringF(t1, aSortProperty).c_str(), aItemHandler.stringF(t2, aSortProperty).c_str());
				break;
			}
			case SORT_CUSTOM: {
				res = aItemHandler.customSorterF(t1, t2, aSortProperty);
				break;
			}
			}

			return aSortAscending == 1 ? res < 0 : res > 0;
		}

		api_return handleGetItems(ApiRequest& aRequest) {
			auto start = aRequest.getRangeParam(1);
			auto end = aRequest.getRangeParam(2);
			decltype(matchingItems) matchingItemsCopy;

			{
				RLock l(cs);
				matchingItemsCopy = matchingItems;
			}

			auto j = Serializer::serializeFromPosition(start, end - start, matchingItemsCopy, [&](const T& i) {
				return Serializer::serializeItem(i, itemHandler);
			});

			aRequest.setResponseBody(j);
			return websocketpp::http::status_code::ok;
		}

		typename ItemList::iterator findItem(const T& aItem, ItemList& aItems) noexcept {
			return find(aItems.begin(), aItems.end(), aItem);
		}

		typename ItemList::const_iterator findItem(const T& aItem, const ItemList& aItems) const noexcept {
			return find(aItems.begin(), aItems.end(), aItem);
		}

		bool isInList(const T& aItem, const ItemList& aItems) const noexcept {
			return findItem(aItem, aItems) != aItems.end();
		}

		int64_t getPosition(const T& aItem, const ItemList& aItems) const noexcept {
			auto i = findItem(aItem, aItems);
			if (i == aItems.end()) {
				return -1;
			}

			return distance(aItems.begin(), i);
		}

		// TASKS START

		class ItemTasks {
		public:
			struct MergeTask {
				int8_t type;
				PropertyIdSet updatedProperties;

				MergeTask(int8_t aType, const PropertyIdSet& aUpdatedProperties = PropertyIdSet()) : type(aType), updatedProperties(aUpdatedProperties) {

				}

				void merge(const MergeTask& aTask) {
					// Ignore
					if (type < aTask.type) {
						return;
					}

					// Merge
					if (type == aTask.type) {
						updatedProperties.insert(aTask.updatedProperties.begin(), aTask.updatedProperties.end());
						return;
					}

					// Replace the task
					type = aTask.type;
					updatedProperties = aTask.updatedProperties;
				}
			};

			typedef map<T, MergeTask> TaskMap;

			void add(const T& aItem, MergeTask&& aData) {
				WLock l(cs);
				auto j = tasks.find(aItem);
				if (j != tasks.end()) {
					(*j).second.merge(aData);
					return;
				}

				tasks.emplace(aItem, move(aData));
			}

			void clear() {
				WLock l(cs);
				tasks.clear();
			}

			bool remove(const T& aItem) {
				WLock l(cs);
				return tasks.erase(aItem) > 0;
			}

			void get(TaskMap& map) {
				WLock l(cs);
				swap(tasks, map);
			}
		private:
			TaskMap tasks;

			SharedMutex cs;
		};

		class ViewTasks : public ItemTasks {
		public:
			void addItem(const T& aItem) {
				tasks.add(aItem, typename ViewTasks::MergeTask(ADD_ITEM));
			}

			void removeItem(const T& aItem) {
				tasks.add(aItem, typename ViewTasks::MergeTask(REMOVE_ITEM));
			}

			void updateItem(const T& aItem, const PropertyIdSet& aUpdatedProperties) {
				updatedProperties.insert(aUpdatedProperties.begin(), aUpdatedProperties.end());
				tasks.add(aItem, typename ViewTasks::MergeTask(UPDATE_ITEM, aUpdatedProperties));
			}

			void get(typename ItemTasks::TaskMap& map, PropertyIdSet& updatedProperties_) {
				tasks.get(map);
				updatedProperties_.swap(updatedProperties);
			}

			void clear() {
				updatedProperties.clear();
				tasks.clear();
			}
		private:
			PropertyIdSet updatedProperties;
			ItemTasks tasks;
		};


		void runTasks() {
			typename ViewTasks::TaskMap currentTasks;
			PropertyIdSet updatedProperties;
			tasks.get(currentTasks, updatedProperties);

			// Anything to update?
			if (currentTasks.empty() && !currentValues.hasChanged() && !itemListChanged) {
				return;
			}

			// Get the updated values
			typename IntCollector::ValueMap updateValues;

			{
				WLock l(cs);
				updateValues = currentValues.getAll();
			}

			// Sorting
			auto sortAscending = updateValues[IntCollector::TYPE_SORT_ASCENDING];
			auto sortProperty = updateValues[IntCollector::TYPE_SORT_PROPERTY];
			if (sortProperty < 0) {
				return;
			}

			maybeSort(updatedProperties, sortProperty, sortAscending);

			// Start position
			auto newStart = updateValues[IntCollector::TYPE_RANGE_START];

			json j;

			// Go through the tasks
			auto updatedItems = handleTasks(currentTasks, sortProperty, sortAscending, newStart);

			if (newStart >= 0) {
				// Get the new visible items
				updateViewItems(updatedItems, j, newStart, updateValues[IntCollector::TYPE_MAX_COUNT]);

				// Append other changed properties
				auto startOffset = newStart - updateValues[IntCollector::TYPE_RANGE_START];
				if (startOffset != 0) {
					j["range_offset"] = startOffset;
				}

				j["range_start"] = newStart;
			}

			// Set cached values
			{
				WLock l(cs);
				prevValues.swap(updateValues);
			}

			// Counts should be updated even if the list doesn't have valid settings posted
			appendItemCounts(j);
			dcassert((matchingItems.size() != 0 && allItems.size() != 0) || currentViewItems.empty());

			sendJson(j);
		}

		typedef std::map<T, const PropertyIdSet&> ItemPropertyIdMap;
		ItemPropertyIdMap handleTasks(const typename ViewTasks::TaskMap& aTaskList, int aSortProperty, int aSortAscending, int& rangeStart_) {
			ItemPropertyIdMap updatedItems;
			for (auto& t : aTaskList) {
				switch (t.second.type) {
				case ADD_ITEM: {
					handleAddItem(t.first, aSortProperty, aSortAscending, rangeStart_);
					break;
				}
				case REMOVE_ITEM: {
					handleRemoveItem(t.first, rangeStart_);
					break;
				}
				case UPDATE_ITEM: {
					if (handleUpdateItem(t.first, aSortProperty, aSortAscending, rangeStart_)) {
						updatedItems.emplace(t.first, t.second.updatedProperties);
					}
					break;
				}
				}
			}

			return updatedItems;
		}

		void updateViewItems(const ItemPropertyIdMap& aUpdatedItems, json& json_, int& newStart_, int aMaxCount) {
			// Get the new visible items
			decltype(currentViewItems) viewItemsNew, oldViewItems;
			{
				RLock l(cs);
				if (newStart_ >= static_cast<int>(allItems.size())) {
					newStart_ = 0;
				}

				auto count = min(static_cast<int>(matchingItems.size()) - newStart_, aMaxCount);
				if (count < 0) {
					return;
				}


				auto startIter = matchingItems.begin();
				advance(startIter, newStart_);

				auto endIter = startIter;
				advance(endIter, count);

				std::copy(startIter, endIter, back_inserter(viewItemsNew));
				oldViewItems = currentViewItems;
			}

			json_["items"] = json::array();

			// List items
			int pos = 0;
			for (const auto& item : viewItemsNew) {
				if (!isInList(item, oldViewItems)) {
					appendItem(item, json_, pos);
				} else {
					// append position
					auto props = aUpdatedItems.find(item);
					if (props != aUpdatedItems.end()) {
						appendItem(item, json_, pos, props->second);
					} else {
						appendItemPosition(item, json_, pos);
					}
				}

				pos++;
			}

			{
				WLock l(cs);
				currentViewItems.swap(viewItemsNew);
			}
		}

		void maybeSort(const PropertyIdSet& aUpdatedProperties, int aSortProperty, int aSortAscending) {
			bool needSort = aUpdatedProperties.find(aSortProperty) != aUpdatedProperties.end() ||
				prevValues[IntCollector::TYPE_SORT_ASCENDING] != aSortAscending ||
				prevValues[IntCollector::TYPE_SORT_PROPERTY] != aSortProperty ||
				itemListChanged;

			itemListChanged = false;

			if (needSort) {
				auto start = GET_TICK();

				WLock l(cs);
				std::sort(matchingItems.begin(), matchingItems.end(),
					std::bind(&ListViewController::itemSort,
						std::placeholders::_1,
						std::placeholders::_2,
						itemHandler,
						aSortProperty,
						aSortAscending
						));

				dcdebug("Table %s sorted in " U64_FMT " ms\n", viewName.c_str(), GET_TICK() - start);
			}
		}

		void appendItemCounts(json& json_) {
			int matchingItemCount = 0, totalItemCount = 0;

			{
				RLock l(cs);
				matchingItemCount = matchingItems.size();
				totalItemCount = allItems.size();
			}

			if (matchingItemCount != prevMatchingItemCount) {
				prevMatchingItemCount = matchingItemCount;
				json_["matching_items"] = matchingItemCount;
			}

			if (totalItemCount != prevTotalItemCount) {
				prevTotalItemCount = totalItemCount;
				json_["total_items"] = totalItemCount;
			}
		}

		void handleAddItem(const T& aItem, int aSortProperty, int aSortAscending, int& rangeStart_) {
			bool matches = matchesFilter(aItem, getFilterMatchers());

			WLock l(cs);
			allItems.emplace(aItem);
			if (matches) {
				auto iter = matchingItems.insert(std::lower_bound(
					matchingItems.begin(),
					matchingItems.end(),
					aItem,
					std::bind(&ListViewController::itemSort, std::placeholders::_1, std::placeholders::_2, itemHandler, aSortProperty, aSortAscending)
					), aItem);

				auto pos = static_cast<int>(std::distance(matchingItems.begin(), iter));
				if (pos < rangeStart_) {
					// Update the range range positions
					rangeStart_++;
				}
			}
		}

		void handleRemoveItem(const T& aItem, int& rangeStart_) {
			WLock l(cs);
			auto iter = findItem(aItem, matchingItems);
			auto pos = static_cast<int>(std::distance(matchingItems.begin(), iter));

			matchingItems.erase(iter);
			allItems.erase(aItem);

			if (rangeStart_ > 0 && pos > rangeStart_) {
				// Update the range range positions
				rangeStart_--;
			}
		}

		// Returns false if the item was added/removed
		bool handleUpdateItem(const T& aItem, int aSortProperty, int aSortAscending, int& rangeStart_) {
			bool inList;

			{
				RLock l(cs);
				inList = isInList(aItem, matchingItems);
			}

			auto matchers = getFilterMatchers();
			if (!matchesFilter(aItem, matchers)) {
				if (inList) {
					handleRemoveItem(aItem, rangeStart_);
				}

				return false;
			} else if (!inList) {
				handleAddItem(aItem, aSortProperty, aSortAscending, rangeStart_);
				return false;
			}

			return true;
		}

		// TASKS END

		// JSON APPEND START
		void appendItem(const T& aItem, json& json_, int pos) {
			appendItem(aItem, json_, pos, toPropertyIdSet(itemHandler.properties));
		}

		void appendItem(const T& aItem, json& json_, int pos, const PropertyIdSet& aPropertyIds) {
			appendItemPosition(aItem, json_, pos);
			json_["items"][pos]["properties"] = Serializer::serializeItemProperties(aItem, aPropertyIds, itemHandler);
		}

		void appendItemPosition(const T& aItem, json& json_, int pos) {
			json_["items"][pos]["id"] = aItem->getToken();
		}

		PropertyFilter::List filters;

		const PropertyItemHandler<T>& itemHandler;

		ItemList currentViewItems;
		ItemList matchingItems;
		std::set<T, std::less<T>> allItems;

		bool active = false;

		SharedMutex cs;

		ApiModule* module = nullptr;
		std::string viewName;

		// Must be in merging order (lower ones replace other)
		enum Tasks {
			UPDATE_ITEM = 0,
			ADD_ITEM,
			REMOVE_ITEM
		};

		TimerPtr timer;

		ViewTasks tasks;

		class IntCollector {
		public:
			enum ValueType {
				TYPE_SORT_PROPERTY,
				TYPE_SORT_ASCENDING,
				TYPE_RANGE_START,
				TYPE_MAX_COUNT,
				TYPE_LAST
			};

			typedef std::map<ValueType, int> ValueMap;

			IntCollector() {
				reset();
			}

			void reset() noexcept {
				for (int i = 0; i < TYPE_LAST; i++) {
					values[static_cast<ValueType>(i)] = -1;
				}
			}

			void set(ValueType aType, int aValue) noexcept {
				changed = true;
				values[aType] = aValue;
			}

			void set(const ValueMap& aMap) noexcept {
				changed = true;
				for (const auto& i : aMap) {
					values[i.first] = i.second;
				}
			}

			ValueMap getAll() noexcept {
				changed = false;
				return values;
			}

			bool hasChanged() const noexcept {
				return changed;
			}
		private:
			bool changed = true;
			ValueMap values;
		};

		StateChangeFunction stateChangeF = nullptr;

		bool itemListChanged = false;
		IntCollector currentValues;

		int prevMatchingItemCount = -1;
		int prevTotalItemCount = -1;
		ItemListF itemListF;
		typename IntCollector::ValueMap prevValues;
	};
}

#endif
