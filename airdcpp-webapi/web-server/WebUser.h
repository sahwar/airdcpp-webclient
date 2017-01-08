/*
* Copyright (C) 2011-2016 AirDC++ Project
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

#ifndef DCPLUSPLUS_DCPP_WEBUSER_H
#define DCPLUSPLUS_DCPP_WEBUSER_H

#include <web-server/stdinc.h>
#include <web-server/Access.h>

#include <airdcpp/typedefs.h>
#include <airdcpp/GetSet.h>

namespace webserver {
	class WebUser;
	typedef std::shared_ptr<WebUser> WebUserPtr;
	typedef vector<WebUserPtr> WebUserList;

	class WebUser {
	public:
		WebUser(const std::string& aUserName, const std::string& aPassword, bool aIsAdmin = false);

		const string& getToken() const noexcept {
			return userName;
		}

		GETSET(std::string, userName, UserName);
		GETSET(std::string , password, Password);
		IGETSET(time_t, lastLogin, LastLogin, 0);

		WebUser(WebUser&) = delete;
		WebUser& operator=(WebUser&) = delete;

		// Sesszions
		int getActiveSessions() const noexcept {
			return activeSessions;
		}

		void addSession() noexcept {
			activeSessions++;
		}

		void removeSession() noexcept {
			activeSessions--;
		}

		// Access
		bool hasPermission(Access aAccess) const noexcept;

		void setPermissions(const string& aStr) noexcept;
		void setPermissions(const StringList& aPermissions) noexcept;

		string getPermissionsStr() const noexcept;
		StringList getPermissions() const noexcept;
		bool isAdmin() const noexcept;

		const static StringList accessStrings;
		static Access toAccess(const string& aStr) noexcept;

		int countPermissions() const noexcept;

		static bool validateUsername(const string& aUsername) noexcept;
	private:
		void clearPermissions() noexcept;
		int activeSessions = 0;

		AccessMap permissions;
	};

}

#endif