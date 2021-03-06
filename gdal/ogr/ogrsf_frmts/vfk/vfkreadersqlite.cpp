/******************************************************************************
 * $Id$
 *
 * Project:  VFK Reader (SQLite)
 * Purpose:  Implements VFKReaderSQLite class.
 * Author:   Martin Landa, landa.martin gmail.com
 *
 ******************************************************************************
 * Copyright (c) 2012, Martin Landa <landa.martin gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 ****************************************************************************/

#include "vfkreader.h"
#include "vfkreaderp.h"

#include "cpl_conv.h"
#include "cpl_error.h"

#define SUPPORT_GEOMETRY

#ifdef SUPPORT_GEOMETRY
#  include "ogr_geometry.h"
#endif

#ifdef HAVE_SQLITE

/*!
  \brief VFKReaderSQLite constructor
*/
VFKReaderSQLite::VFKReaderSQLite(const char *pszFilename) : VFKReader(pszFilename)
{
    CPLString   pszDbName(m_pszFilename);
    CPLString   osCommand;
    VSIStatBufL sStatBuf;
    bool        bNewDb;
    
    /* open tmp SQLite DB (re-use DB file if already exists) */
    pszDbName += ".db";

    bNewDb = TRUE;
    if (VSIStatL(pszDbName, &sStatBuf ) == 0) {
        CPLDebug("OGR-VFK", "Reading DB '%s'", pszDbName.c_str());
        bNewDb = FALSE;
    }
    
    if (SQLITE_OK != sqlite3_open(pszDbName, &m_poDB)) {
        CPLError(CE_Failure, CPLE_AppDefined, 
                 "Creating SQLite DB failed");
    }
    else {
        char* pszErrMsg = NULL;
        sqlite3_exec(m_poDB, "PRAGMA synchronous = OFF", NULL, NULL, &pszErrMsg);
        sqlite3_free(pszErrMsg);
    }
    
    if (bNewDb) {
        /* new DB, create support metadata tables */
        osCommand.Printf("CREATE TABLE 'vfk_blocks' "
                         "(file_name text, table_name text, "
                         "num_records integer, table_defn text);");
        ExecuteSQL(osCommand.c_str());
    }
}

/*!
  \brief VFKReaderSQLite destructor
*/
VFKReaderSQLite::~VFKReaderSQLite()
{
    CPLString pszDbName(m_pszFilename);

    pszDbName += ".db";
    
    /* close tmp SQLite DB */
    if (SQLITE_OK != sqlite3_close(m_poDB)) {
        CPLError(CE_Failure, CPLE_AppDefined, 
                 "Closing SQLite DB failed\n  %s",
                 sqlite3_errmsg(m_poDB));
    }
}

/*!
  \brief Load data block definitions (&B)

  Call VFKReader::OpenFile() before this function.

  \return number of data blocks
  \return -1 on error
*/
int VFKReaderSQLite::ReadDataBlocks()
{
    int  nDataBlocks;
    CPLString osSQL;
    const char *pszName, *pszDefn;
    IVFKDataBlock *poNewDataBlock;
    
    sqlite3_stmt *hStmt;
    
    osSQL.Printf("SELECT table_name, table_defn FROM 'vfk_blocks' WHERE "
                 "file_name = '%s'", m_pszFilename);
    hStmt = PrepareStatement(osSQL.c_str());
    while(ExecuteSQL(hStmt) == OGRERR_NONE) {
        pszName = (const char*) sqlite3_column_text(hStmt, 0);
        pszDefn = (const char*) sqlite3_column_text(hStmt, 1);
        poNewDataBlock = (IVFKDataBlock *) CreateDataBlock(pszName);
        poNewDataBlock->SetGeometryType();
        poNewDataBlock->SetProperties(pszDefn);
        VFKReader::AddDataBlock(poNewDataBlock, NULL);
    }
    
    if (m_nDataBlockCount == 0) {
        sqlite3_exec(m_poDB, "BEGIN", 0, 0, 0);  
        /* CREATE TABLE ... */
        nDataBlocks = VFKReader::ReadDataBlocks();
        sqlite3_exec(m_poDB, "COMMIT", 0, 0, 0);
    }
    
    return nDataBlocks;
}

/*!
  \brief Load data records (&D)

  Call VFKReader::OpenFile() before this function.
  
  \return number of data records
  \return -1 on error
*/
int VFKReaderSQLite::ReadDataRecords(IVFKDataBlock *poDataBlock)
{
    int         nDataRecords;
    const char *pszName;
    CPLString   osSQL;

    sqlite3_stmt *hStmt;
    
    /* table name */
    pszName = poDataBlock->GetName();
    
    /* check for existing records (re-use already inserted data) */
    osSQL.Printf("SELECT num_records, table_defn FROM 'vfk_blocks' WHERE "
                 "file_name = '%s' AND table_name = '%s'",
                 m_pszFilename, pszName);
    hStmt = PrepareStatement(osSQL.c_str());
    nDataRecords = 0;
    if (ExecuteSQL(hStmt) == OGRERR_NONE) {
        nDataRecords = sqlite3_column_int(hStmt, 0);
    }
    sqlite3_finalize(hStmt);

    if (nDataRecords > 0) {
        VFKFeatureSQLite *poNewFeature;
        
        poDataBlock->SetFeatureCount(0);
        poDataBlock->SetMaxFID(0);
        for (int i = 0; i < nDataRecords; i++) {
            poNewFeature = new VFKFeatureSQLite(poDataBlock);
            poDataBlock->AddFeature(poNewFeature);
        }
        poDataBlock->SetMaxFID(poNewFeature->GetFID()); /* update max value */
    }
    else {
        sqlite3_exec(m_poDB, "BEGIN", 0, 0, 0);
        
        /* INSERT ... */
        nDataRecords = VFKReader::ReadDataRecords(poDataBlock);
        
        /* update 'vfk_blocks' table */
        osSQL.Printf("UPDATE 'vfk_blocks' SET num_records = %d WHERE file_name = '%s' AND table_name = '%s'",
                     nDataRecords, m_pszFilename, pszName);
        sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
        
        /* create indeces */
        osSQL.Printf("CREATE UNIQUE INDEX %s_ID ON '%s' (ID)",
                     pszName, pszName);
        sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
        
        if (EQUAL(pszName, "SBP")) {
            /* create extra indices for SBP */
            osSQL.Printf("CREATE UNIQUE INDEX SBP_OB ON '%s' (OB_ID)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
            osSQL.Printf("CREATE UNIQUE INDEX SBP_HP ON '%s' (HP_ID)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
            osSQL.Printf("CREATE UNIQUE INDEX SBP_DPM ON '%s' (DPM_ID)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
            osSQL.Printf("CREATE UNIQUE INDEX SBP_OB_HP_DPM ON '%s' (OB_ID,HP_ID,DPM_ID)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
            osSQL.Printf("CREATE UNIQUE INDEX SBP_HP_POR ON '%s' (HP_ID,PORADOVE_CISLO_BODU)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
            osSQL.Printf("CREATE UNIQUE INDEX SBP_DPM_POR ON '%s' (DPM_ID,PORADOVE_CISLO_BODU)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
        }
        else if (EQUAL(pszName, "HP")) {
            /* create extra indices for HP */
            osSQL.Printf("CREATE UNIQUE INDEX HP_PAR1 ON '%s' (PAR_ID_1)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
            osSQL.Printf("CREATE UNIQUE INDEX HP_PAR2 ON '%s' (PAR_ID_2)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
        }
        else if (EQUAL(pszName, "OP")) {
            /* create extra indices for OP */
            osSQL.Printf("CREATE UNIQUE INDEX OP_BUD ON '%s' (BUD_ID)",
                         pszName);
            sqlite3_exec(m_poDB, osSQL.c_str(), 0, 0, 0);
            
        }
        
        sqlite3_exec(m_poDB, "COMMIT", 0, 0, 0);
    }
        
    return nDataRecords;
}

IVFKDataBlock *VFKReaderSQLite::CreateDataBlock(const char *pszBlockName)
{
    return new VFKDataBlockSQLite(pszBlockName, (IVFKReader *) this);
}

/*!
  \brief Create DB table from VFKDataBlock (SQLITE only)

  \param poDataBlock pointer to VFKDataBlock instance
*/
void VFKReaderSQLite::AddDataBlock(IVFKDataBlock *poDataBlock, const char *pszDefn)
{
    CPLString osCommand, osColumn;
    VFKPropertyDefn *poPropertyDefn;
    
    sqlite3_stmt *hStmt;

    /* register table in 'vfk_blocks' */
    osCommand.Printf("SELECT COUNT(*) FROM 'vfk_blocks' WHERE "
                     "file_name = '%s' AND table_name = '%s'",
                     m_pszFilename, poDataBlock->GetName());
    hStmt = PrepareStatement(osCommand.c_str());
    if (ExecuteSQL(hStmt) == OGRERR_NONE &&
        sqlite3_column_int(hStmt, 0) == 0) {
        
        osCommand.Printf("CREATE TABLE '%s' (", poDataBlock->GetName());
        for (int i = 0; i < poDataBlock->GetPropertyCount(); i++) {
            poPropertyDefn = poDataBlock->GetProperty(i);
            if (i > 0)
                osCommand += ",";
            osColumn.Printf("%s %s", poPropertyDefn->GetName(),
                            poPropertyDefn->GetTypeSQL().c_str());
            osCommand += osColumn;
        }
        osCommand += ",ogr_fid INTERGER);";
        
        ExecuteSQL(osCommand.c_str()); /* CREATE TABLE */

        osCommand.Printf("INSERT INTO 'vfk_blocks' (file_name, table_name, "
                         "num_records, table_defn) VALUES ('%s', '%s', 0, '%s')",
                         m_pszFilename, poDataBlock->GetName(), pszDefn);
        ExecuteSQL(osCommand.c_str());

        sqlite3_finalize(hStmt);
    }
        
    return VFKReader::AddDataBlock(poDataBlock, NULL);
}

/*!
  \brief Prepare SQL statement
  
  \param pszSQLCommand SQL statement to be prepared

  \return pointer to sqlite3_stmt instance
  \return NULL on error
*/
sqlite3_stmt *VFKReaderSQLite::PrepareStatement(const char *pszSQLCommand)
{
    int rc;
    sqlite3_stmt *hStmt = NULL;
    
    rc = sqlite3_prepare(m_poDB, pszSQLCommand, strlen(pszSQLCommand),
                         &hStmt, NULL);
    
    if (rc != SQLITE_OK) {
        CPLError(CE_Failure, CPLE_AppDefined, 
                 "In PrepareStatement(): sqlite3_prepare(%s):\n  %s",
                 pszSQLCommand, sqlite3_errmsg(m_poDB));
        
        if(hStmt != NULL) {
            sqlite3_finalize(hStmt);
        }
        
        return NULL;
    }

    return hStmt;
}

/*!
  \brief Execute prepared SQL statement

  \param hStmt pointer to sqlite3_stmt 

  \return OGRERR_NONE on success
*/
OGRErr VFKReaderSQLite::ExecuteSQL(sqlite3_stmt *hStmt)
{
    int rc;
    
    // assert

    rc = sqlite3_step(hStmt);
    if (rc != SQLITE_ROW) {
        if (rc == SQLITE_DONE) {
            sqlite3_finalize(hStmt);
            return OGRERR_NOT_ENOUGH_DATA;
        }
        
        CPLError(CE_Failure, CPLE_AppDefined, 
                 "In ExecuteSQL(): sqlite3_step:\n  %s", 
                 sqlite3_errmsg(m_poDB));
        if (hStmt)
            sqlite3_finalize(hStmt);
        return OGRERR_FAILURE;
    }
    
    return OGRERR_NONE;

}

/*!
  \brief Execute SQL statement (SQLITE only)

  \return OGRERR_NONE on success
  \return OGRERR_FAILURE on failure
*/
OGRErr VFKReaderSQLite::ExecuteSQL(const char *pszSQLCommand)
{
    int rc;
    sqlite3_stmt *hSQLStmt = NULL;

    rc = sqlite3_prepare(m_poDB, pszSQLCommand, strlen(pszSQLCommand),
                         &hSQLStmt, NULL);
    
    if (rc != SQLITE_OK) {
        CPLError(CE_Failure, CPLE_AppDefined, 
                 "In ExecuteSQL(): sqlite3_prepare(%s):\n  %s",
                 pszSQLCommand, sqlite3_errmsg(m_poDB));
        
        if(hSQLStmt != NULL) {
            sqlite3_finalize(hSQLStmt);
        }
        
        return OGRERR_FAILURE;
    }

    rc = sqlite3_step(hSQLStmt);
    if (rc != SQLITE_DONE) {
        CPLError(CE_Failure, CPLE_AppDefined, 
                 "In ExecuteSQL(): sqlite3_step(%s):\n  %s", 
                 pszSQLCommand, sqlite3_errmsg(m_poDB));
        
        sqlite3_finalize(hSQLStmt);
        
        return OGRERR_FAILURE;
    }

    sqlite3_finalize(hSQLStmt);
    
    return OGRERR_NONE;
}

/*!
  \brief Add feature

  \param poNewDataBlock pointer to VFKDataBlock instance
  \param poNewFeature pointer to VFKFeature instance
*/
void VFKReaderSQLite::AddFeature(IVFKDataBlock *poDataBlock, VFKFeature *poFeature)
{
    CPLString     osCommand;
    CPLString     osValue;
    
    OGRFieldType  ftype;

    VFKFeatureSQLite  *poNewFeature;
    const VFKProperty *poProperty;
    
    osCommand.Printf("INSERT INTO '%s' VALUES(", poDataBlock->GetName());
    
    for (int i = 0; i < poDataBlock->GetPropertyCount(); i++) {
        ftype = poDataBlock->GetProperty(i)->GetType();
        poProperty = poFeature->GetProperty(i);
        if (i > 0)
            osCommand += ",";
        if (poProperty->IsNull())
            osValue.Printf("NULL");
        else {
            switch (ftype) {
            case OFTInteger:
                osValue.Printf("%d", poProperty->GetValueI());
                break;
            case OFTReal:
                osValue.Printf("%f", poProperty->GetValueD());
                break;
            case OFTString:
                if (poDataBlock->GetProperty(i)->IsIntBig())
                    osValue.Printf("%lu", strtoul(poProperty->GetValueS(), NULL, 0));
                else
                    osValue.Printf("'%s'", poProperty->GetValueS());
                break;
            default:
                osValue.Printf("'%s'", poProperty->GetValueS());
                break;
            }
        }
        osCommand += osValue;
    }
    osValue.Printf(",%lu);", poFeature->GetFID());
    osCommand += osValue;
    
    ExecuteSQL(osCommand.c_str());

    poNewFeature = new VFKFeatureSQLite(poFeature);
    poDataBlock->AddFeature(poNewFeature);
    delete poFeature;
}

#endif /* HAVE_SQLITE */
