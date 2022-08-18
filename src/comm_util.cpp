/***************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Implement comm_util.h -- communication driver utilities
 * Author:   David Register, Alec Leamas
 *
 ***************************************************************************
 *   Copyright (C) 2022 by David Register, Alec Leamas                     *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301,  USA.         *
 **************************************************************************/
#include <vector>

// FIXME  Why is this needed?
#ifdef __MSVC__
#include "winsock2.h"
#include "wx/msw/winundef.h"
#endif

#include "wx/wxprec.h"

#ifndef WX_PRECOMP
#include "wx/string.h"
#endif  // precompiled headers

#include "comm_util.h"
#include "comm_drv_registry.h"

bool StopAndRemoveCommDriver(std::string ident) {
  auto& registry = CommDriverRegistry::getInstance();
  const std::vector<DriverPtr>& drivers = registry.GetDrivers();
  DriverPtr target_driver = FindDriver(drivers, ident);

  if (!target_driver) return false;

  // Deactivate the driver, and the last reference in shared_ptr
  // will be removed.
  // The driver DTOR will be called in due course.
  registry.Deactivate(target_driver);

  return true;
}

//----------------------------------------------------------------------------------
//     Strip NMEA V4 tags from NMEA0183 message
//----------------------------------------------------------------------------------
wxString ProcessNMEA4Tags(wxString& msg) {
  int idxFirst = msg.Find('\\');

  if (wxNOT_FOUND == idxFirst) return msg;

  if (idxFirst < (int)msg.Length() - 1) {
    int idxSecond = msg.Mid(idxFirst + 1).Find('\\') + 1;
    if (wxNOT_FOUND != idxSecond) {
      if (idxSecond < (int)msg.Length() - 1) {
        // wxString tag = msg.Mid(idxFirst+1, (idxSecond - idxFirst) -1);
        return msg.Mid(idxSecond + 1);
      }
    }
  }

  return msg;
}
