/******************************************************************************
 *
 * Project:  OpenCPN
 * Purpose:  Chart Database Object
 * Author:   David Register
 *
 ***************************************************************************
 *   Copyright (C) 2010 by David S. Register   *
 *   bdbcat@yahoo.com   *
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
 *   59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.             *
 ***************************************************************************
 *
 *
 *
 */

#ifndef __CHARTDB_H__
#define __CHARTDB_H__


#include "wx/file.h"
#include "wx/stream.h"
#include "wx/wfstream.h"
#include "wx/tokenzr.h"
#include "wx/dir.h"
#include "wx/filename.h"
#include <wx/xml/xml.h>

#include "chartbase.h"
#include "chartdbs.h"

#define     MAXSTACK          100

#include "s52s57.h"           //types


// ----------------------------------------------------------------------------
//    Constants, etc.
// ----------------------------------------------------------------------------

typedef struct  {
    float y;
    float x;
} MyFlPoint;


// ----------------------------------------------------------------------------
//    Fwd Declarations
// ----------------------------------------------------------------------------
class ChartBase;

// ----------------------------------------------------------------------------
// ----------------------------------------------------------------------------

class ChartStack
{
public:
      ChartStack() { nEntry = 0; CurrentStackEntry = 0; b_valid = false;}

      bool        b_valid;
      int         nEntry;
      int         CurrentStackEntry;
      int         GetCurrentEntrydbIndex(void);
      void        SetCurrentEntryFromdbIndex(int current_db_index);
      int         GetDBIndex(int stack_index);
      void        SetDBIndex(int stack_index, int db_index);
      bool        DoesStackContaindbIndex(int db_index);

private:
      int         DBIndex[MAXSTACK];


};

class CacheEntry
{
public:
      wxString    FullPath;
      void        *pChart;
      int         RecentTime;
      int         dbIndex;
};



// ----------------------------------------------------------------------------
// Chart Database
// ----------------------------------------------------------------------------

class ChartDB: public ChartDatabase
{
public:

      ChartDB(MyFrame *parent);
      virtual ~ChartDB();


      bool LoadBinary(wxString *filename) { return ChartDatabase::Read(*filename); }
      bool SaveBinary(wxString *filename) { return ChartDatabase::Write(*filename); }

      int  BuildChartStack(ChartStack * cstk, float lat, float lon);
      bool EqualStacks(ChartStack *, ChartStack *);
      bool CopyStack(ChartStack *pa, ChartStack *pb);
      wxString GetFullPath(ChartStack *ps, int stackindex);
      int  GetStackChartScale(ChartStack *ps, int stackindex, char *buf, int nbuf);
      int  GetCSPlyPoint(ChartStack *ps, int stackindex, int plyindex, float *lat, float *lon);
      ChartTypeEnum GetCSChartType(ChartStack *ps, int stackindex);
      ChartFamilyEnum GetCSChartFamily(ChartStack *ps, int stackindex);
      bool SearchForChartDir(wxString &dir);
      ChartBase *OpenStackChartConditional(ChartStack *ps, int start_index, bool bLargest, ChartTypeEnum New_Type, ChartFamilyEnum New_Family_Fallback);

      wxArrayPtrVoid *GetChartCache(void) { return pChartCache; }
      ArrayOfInts GetCSArray(ChartStack *ps);

      int GetStackEntry(ChartStack *ps, wxString fp);
      bool IsChartInCache(int dbindex);

      ChartBase *OpenChartFromStack(ChartStack *pStack, int StackEntry, ChartInitFlag iflag = FULL_INIT);
      ChartBase *OpenChartFromDB(int index, ChartInitFlag init_flag);

      void ApplyColorSchemeToCachedCharts(ColorScheme cs);
      void PurgeCache();
      bool DeleteCacheChart(ChartBase *pChart);

      void LockCache(bool bl){m_b_locked = bl;}
      void LockCache(){m_b_locked = true;}
      void UnLockCache(){m_b_locked = false;}
      bool IsCacheLocked(){ return m_b_locked; }
      wxXmlDocument GetXMLDescription(int dbIndex, bool b_getGeom);


protected:
      virtual ChartBase *GetChart(const wxChar *theFilePath, ChartClassDescriptor &chart_desc) const;

private:
      InitReturn CreateChartTableEntry(wxString full_name, ChartTableEntry *pEntry);

      int SearchDirAndAddSENC(wxString& dir, bool bshow_prog, bool bupdate);
      bool CreateS57SENCChartTableEntry(wxString full_name, ChartTableEntry *pEntry, Extent *pext);
      bool CheckPositionWithinChart(int index, float lat, float lon);
      ChartBase *OpenChartUsingCache(int dbindex, ChartInitFlag init_flag);

      wxArrayPtrVoid    *pChartCache;

      MyFrame           *pParent;
      bool              m_b_locked;

};


#endif

