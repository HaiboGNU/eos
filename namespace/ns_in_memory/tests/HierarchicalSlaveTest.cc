//------------------------------------------------------------------------------
// author: Lukasz Janyst <ljanyst@cern.ch>
// desc:   HierarchicalView test
//------------------------------------------------------------------------------
// EOS - the CERN Disk Storage System
// Copyright (C) 2011 CERN/Switzerland
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//------------------------------------------------------------------------------

#include <cppunit/extensions/HelperMacros.h>

#include <stdint.h>
#include <unistd.h>
#include <sstream>
#include <cstdlib>
#include <ctime>

#include "namespace/utils/Locking.hh"
#include "namespace/utils/TestHelpers.hh"
#include "namespace/ns_in_memory/views/HierarchicalView.hh"
#include "namespace/ns_in_memory/accounting/QuotaStats.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogContainerMDSvc.hh"
#include "namespace/ns_in_memory/persistency/ChangeLogFileMDSvc.hh"
#include "namespace/ns_in_memory/persistency/LogManager.hh"
#include "namespace/ns_in_memory/accounting/FileSystemView.hh"

#include <XrdSys/XrdSysPthread.hh>

//------------------------------------------------------------------------------
// Declaration
//------------------------------------------------------------------------------
class HierarchicalSlaveTest: public CppUnit::TestCase
{
  public:
    CPPUNIT_TEST_SUITE(HierarchicalSlaveTest);
    CPPUNIT_TEST(functionalTest);
    CPPUNIT_TEST_SUITE_END();

    void functionalTest();
};

CPPUNIT_TEST_SUITE_REGISTRATION(HierarchicalSlaveTest);

//------------------------------------------------------------------------------
// Lock handler
//------------------------------------------------------------------------------
class RWLock: public eos::LockHandler
{
  public:
    //--------------------------------------------------------------------------
    // Constructor
    //--------------------------------------------------------------------------
    RWLock()
    {
    }

    //------------------------------------------------------------------------
    // Destructor
    //------------------------------------------------------------------------
    virtual ~RWLock()
    {
    }

    //------------------------------------------------------------------------
    // Take a read lock
    //------------------------------------------------------------------------
    virtual void readLock()
    {
      pLock.ReadLock();
    }

    //------------------------------------------------------------------------
    // Take a write lock
    //------------------------------------------------------------------------
    virtual void writeLock()
    {
      pLock.WriteLock();
    }

    //------------------------------------------------------------------------
    // Unlock
    //------------------------------------------------------------------------
    virtual void unLock()
    {
      pLock.UnLock();
    }
  private:
    XrdSysRWLock pLock;
};

//------------------------------------------------------------------------------
// Add some replicas to the files inside of a container
//------------------------------------------------------------------------------
void addReplicas(eos::IView* view, eos::IContainerMD* cont)
{
  eos::IFileMD* fmd = 0;
  std::set<std::string> fnames = cont->getNameFiles();

  for (auto fit = fnames.begin(); fit != fnames.end(); ++fit)
  {
    fmd = cont->findFile(*fit);

    for (int i = 0; i < random() % 10; ++i)
      fmd->addLocation(random() % 10);

    view->updateFileStore(fmd);
  }
}

//------------------------------------------------------------------------------
// Unlink some replicas from the files in the container
//------------------------------------------------------------------------------
void unlinkReplicas(eos::IView* view, eos::IContainerMD* cont)
{
  eos::IFileMD* fmd = 0;
  std::set<std::string> fnames = cont->getNameFiles();

  for (auto fit = fnames.begin(); fit != fnames.end(); ++fit)
  {
    fmd = cont->findFile(*fit);
    eos::IFileMD::LocationVector::const_iterator itL;
    eos::IFileMD::LocationVector toUnlink;
    int n = random() % 3;
    int i = 0;
    eos::IFileMD::LocationVector loc_vect = fmd->getLocations();

    for (itL = loc_vect.begin(); itL != loc_vect.end() && i < n; ++itL, ++i)
    {
      toUnlink.push_back(*itL);
    }

    for (itL = toUnlink.begin(); itL != toUnlink.end(); ++itL)
      fmd->unlinkLocation(*itL);

    view->updateFileStore(fmd);
  }
}

//------------------------------------------------------------------------------
// Recursively remove the files from accounting
//------------------------------------------------------------------------------
void cleanUpQuotaRec(eos::IView* view, eos::IContainerMD* cont)
{
  eos::IQuotaNode* qn = view->getQuotaNode(cont);
  std::set<std::string> fnames = cont->getNameFiles();

  for (auto fit = fnames.begin(); fit != fnames.end(); ++fit)
  {
    qn->removeFile(cont->findFile(*fit));
  }

  std::set<std::string> dnames = cont->getNameContainers();

  for (auto dit = dnames.begin(); dit != dnames.end(); ++dit)
  {
    unlinkReplicas(view, cont->findContainer(*dit));
  }
}

//------------------------------------------------------------------------------
// Delete some replicas from the files in the container
//------------------------------------------------------------------------------
void deleteReplicas(eos::IView* view, eos::IContainerMD* cont)
{
  eos::IFileMD* fmd = 0;
  std::set<std::string> fnames = cont->getNameFiles();

  for (auto fit = fnames.begin(); fit != fnames.end(); ++fit)
  {
    fmd = cont->findFile(*fit);
    eos::IFileMD::LocationVector::const_iterator itL;
    eos::IFileMD::LocationVector toDelete;
    int n = random() % 3;
    int i = 0;
    eos::IFileMD::LocationVector unlink_vect = fmd->getUnlinkedLocations();

    for (itL = unlink_vect.begin(); itL != unlink_vect.end() && i < n; ++itL, ++i)
      toDelete.push_back(*itL);

    for (itL = toDelete.begin(); itL != toDelete.end(); ++itL)
      fmd->removeLocation(*itL);

    view->updateFileStore(fmd);
  }
}

//------------------------------------------------------------------------------
// Delete all replicas from all the files in the container recursively
//------------------------------------------------------------------------------
void deleteAllReplicas(eos::IView* view, eos::IContainerMD* cont)
{
  eos::IFileMD* fmd = 0;
  std::set<std::string> fnames = cont->getNameFiles();

  for (auto fit = fnames.begin(); fit != fnames.end(); ++fit)
  {
    fmd = cont->findFile(*fit);
    eos::IFileMD::LocationVector::const_iterator itL;
    eos::IFileMD::LocationVector toDelete = fmd->getLocations();

    for (itL = toDelete.begin(); itL != toDelete.end(); ++itL)
      fmd->unlinkLocation(*itL);

    view->updateFileStore(fmd);

    for (itL = toDelete.begin(); itL != toDelete.end(); ++itL)
      fmd->removeLocation(*itL);

    view->updateFileStore(fmd);
  }
}

//------------------------------------------------------------------------------
// Delete all replicas recursively
//------------------------------------------------------------------------------
void deleteAllReplicasRec(eos::IView* view, eos::IContainerMD* cont)
{
  deleteAllReplicas(view, cont);
  eos::IContainerMD* dmd = 0;
  std::set<std::string> dnames = cont->getNameContainers();

  for (auto dit = dnames.begin(); dit != dnames.end(); ++dit)
  {
    dmd = cont->findContainer(*dit);
    deleteAllReplicasRec(view, dmd);
  }
}

//------------------------------------------------------------------------------
// Delete all replicas recursively
//------------------------------------------------------------------------------
void deleteAllReplicasRec(eos::IView* view, const std::string& path)
{
  eos::IContainerMD* container = view->getContainer(path);
  deleteAllReplicasRec(view, container);
}

//------------------------------------------------------------------------------
// Create the directory subtree
//------------------------------------------------------------------------------
void createSubTree(eos::IView*        view,
                   const std::string& prefix,
                   int                depth,
                   int                numDirs,
                   int                numFiles)
{
  if (!depth) return;

  for (int i = 0; i < numDirs; ++i)
  {
    std::ostringstream o;
    o << prefix << "/dir" << i;
    view->createContainer(o.str(), true);
    createSubTree(view, o.str(), depth - 1, numDirs, numFiles);
  }

  eos::IContainerMD* container = view->getContainer(prefix);
  eos::IQuotaNode*   qn        = view->getQuotaNode(container);

  for (int i = 0; i < numFiles; ++i)
  {
    std::ostringstream o;
    o << prefix << "/file" << i;
    eos::IFileMD* file = view->createFile(o.str());

    if (qn) qn->addFile(file);
  }

  addReplicas(view, container);
}

//------------------------------------------------------------------------------
// Modify subtree
//------------------------------------------------------------------------------
void modifySubTree(eos::IView* view, const std::string& root)
{
  for (int i = 0; i < 5; ++i)
  {
    std::ostringstream o;
    o << root << "/dir" << i;
    eos::IContainerMD* cont = view->getContainer(o.str());
    eos::IQuotaNode* qn = view->getQuotaNode(cont);
    std::vector<eos::IFileMD*> toDel;
    std::vector<eos::IFileMD*>::iterator itD;
    std::set<std::string>::iterator fit;
    eos::IFileMD* fmd = 0;
    std::set<std::string> fnames = cont->getNameFiles();
    int j;

    for (j = 1, fit = fnames.begin() ; fit != fnames.end(); ++fit, ++j)
    {
      fmd = cont->findFile(*fit);
      if (qn) qn->removeFile(fmd);
      fmd->setSize(random() % 1000000);
      if (qn) qn->addFile(fmd);
      view->updateFileStore(fmd);

      if (j % 4 == 0)
        toDel.push_back(fmd);
    }

    for (itD = toDel.begin(); itD != toDel.end(); ++itD)
      view->unlinkFile(view->getUri(*itD));

    while (1)
    {
      bool locationsLeft = false;

      for (itD = toDel.begin(); itD != toDel.end(); ++itD)
      {
        fmd = *itD;

        if (fmd->getNumUnlinkedLocation() == 0)
          continue;

        // Remove just one location at a time
        eos::IFileMD::LocationVector loc_vect = fmd->getUnlinkedLocations();

        for (auto it = loc_vect.begin(); it != loc_vect.end(); ++it)
        {
          fmd->removeLocation(*it);
          break;
        }

        view->updateFileStore(fmd);

        if (fmd->getNumUnlinkedLocation())
          locationsLeft = true;
      }

      if (!locationsLeft)
        break;

      sleep(1);
    }

    for (itD = toDel.begin(); itD != toDel.end(); ++itD)
    {
      fmd = *itD;
      if (qn) qn->removeFile(fmd);
      view->removeFile(fmd);
    }
  }
}

//------------------------------------------------------------------------------
// Calculate total size
//------------------------------------------------------------------------------
uint64_t calcSize(eos::IContainerMD* cont)
{
  uint64_t size = 0;
  eos::IFileMD* fmd = 0;
  eos::IContainerMD* dmd = 0;
  std::set<std::string> fnames = cont->getNameFiles();

  for (auto fit = fnames.begin(); fit != fnames.end(); ++fit)
  {
    fmd = cont->findFile(*fit);
    size += fmd->getSize();
  }

  std::set<std::string> dnames = cont->getNameContainers();

  for (auto dit = dnames.begin(); dit != dnames.end(); ++dit)
  {
    dmd = cont->findContainer(*dit);
    size += calcSize(dmd);
  }

  return size;
}

//------------------------------------------------------------------------------
// Calculate number of files
//------------------------------------------------------------------------------
uint64_t calcFiles(eos::IContainerMD* cont)
{
  eos::IContainerMD* dmd = 0;
  uint64_t files = cont->getNumFiles();
  std::set<std::string> dnames = cont->getNameContainers();

  for (auto dit = dnames.begin(); dit != dnames.end(); ++dit)
  {
    dmd = cont->findContainer(*dit);
    files += calcFiles(dmd);
  }

  return files;
}

//------------------------------------------------------------------------------
// Compare trees
//------------------------------------------------------------------------------
bool compareTrees(eos::IView*        view1, eos::IView*       view2,
                  eos::IContainerMD* tree1, eos::IContainerMD* tree2)
{
  std::string treeMsg = view1->getUri(tree1) + " " + view2->getUri(tree2);
  std::ostringstream o1, o2, o3, o4;
  o1 << " -- " << tree1->getId() << " " << tree2->getId();
  CPPUNIT_ASSERT_MESSAGE(treeMsg + o1.str(),
                         tree1->getId() == tree2->getId());
  o2 << " -- " << tree1->getName() << " " << tree2->getName();
  CPPUNIT_ASSERT_MESSAGE(treeMsg + o2.str(),
                         tree1->getName() == tree2->getName());
  o3 << " -- " << tree1->getNumFiles() << " " << tree2->getNumFiles();
  CPPUNIT_ASSERT_MESSAGE(treeMsg + o3.str(),
                         tree1->getNumFiles() == tree2->getNumFiles());
  o4 << " -- " << tree1->getNumContainers() << " " << tree2->getNumContainers();
  CPPUNIT_ASSERT_MESSAGE(treeMsg + o4.str(),
                         tree1->getNumContainers() == tree2->getNumContainers());

  eos::IFileMD* fmd = 0;
  std::set<std::string> fnames = tree1->getNameFiles();

  for (auto fit = fnames.begin(); fit != fnames.end(); ++fit)
  {
    fmd = tree1->findFile(*fit);
    eos::IFileMD* file = tree2->findFile(fmd->getName());
    std::string fileMsg = treeMsg + " file " + fmd->getName();
    CPPUNIT_ASSERT_MESSAGE(fileMsg + " missing", file);
    CPPUNIT_ASSERT_MESSAGE(fileMsg + " wrong size",
                           file->getSize() == fmd->getSize());
    CPPUNIT_ASSERT_MESSAGE(fileMsg + " wrong id",
                           file->getId() == fmd->getId());
    CPPUNIT_ASSERT_MESSAGE(fileMsg, file->getFileMDSvc());
    CPPUNIT_ASSERT_MESSAGE(fileMsg, fmd->getFileMDSvc());
  }

  eos::IContainerMD* dmd = 0;
  std::set<std::string> dnames = tree1->getNameContainers();

  for (auto dit = dnames.begin(); dit != dnames.end(); ++dit)
  {
    dmd = tree1->findContainer(*dit);
    eos::IContainerMD* container = tree2->findContainer(dmd->getName());
    std::string contMsg = treeMsg + " container: " + dmd->getName();
    CPPUNIT_ASSERT_MESSAGE(contMsg, container);
    compareTrees(view1, view2, dmd, container);
  }

  return true;
}

//------------------------------------------------------------------------------
// Compare filesystems
//------------------------------------------------------------------------------
void compareFileSystems(eos::FileSystemView* viewMaster,
                        eos::FileSystemView* viewSlave)
{
  CPPUNIT_ASSERT(viewMaster->getNumFileSystems() ==
                 viewSlave->getNumFileSystems());

  for (size_t i = 0; i != viewMaster->getNumFileSystems(); ++i)
  {
    CPPUNIT_ASSERT(viewMaster->getFileList(i).size() ==
                   viewSlave->getFileList(i).size());
    CPPUNIT_ASSERT(viewMaster->getUnlinkedFileList(i).size() ==
                   viewSlave->getUnlinkedFileList(i).size());
  }

  CPPUNIT_ASSERT(viewMaster->getNoReplicasFileList().size() ==
                 viewSlave->getNoReplicasFileList().size());
}

//------------------------------------------------------------------------------
// File size mapping function
//------------------------------------------------------------------------------
static uint64_t mapSize(const eos::IFileMD* file)
{
  return file->getSize();
}

//------------------------------------------------------------------------------
// Slave test
//------------------------------------------------------------------------------
void HierarchicalSlaveTest::functionalTest()
{
  srandom(time(0));
  //----------------------------------------------------------------------------
  // Set up the master namespace
  //----------------------------------------------------------------------------
  eos::ChangeLogContainerMDSvc* contSvcMaster = new eos::ChangeLogContainerMDSvc;
  eos::ChangeLogFileMDSvc*      fileSvcMaster = new eos::ChangeLogFileMDSvc;
  eos::IView*                   viewMaster    = new eos::HierarchicalView;
  fileSvcMaster->setContainerService(contSvcMaster);
  std::map<std::string, std::string> fileSettings1;
  std::map<std::string, std::string> contSettings1;
  std::map<std::string, std::string> settings1;
  std::string fileNameFileMD = getTempName("/tmp", "eosns");
  std::string fileNameContMD = getTempName("/tmp", "eosns");
  contSettings1["changelog_path"] = fileNameContMD;
  fileSettings1["changelog_path"] = fileNameFileMD;
  fileSvcMaster->configure(fileSettings1);
  contSvcMaster->configure(contSettings1);
  viewMaster->setContainerMDSvc(contSvcMaster);
  viewMaster->setFileMDSvc(fileSvcMaster);
  viewMaster->configure(settings1);
  viewMaster->getQuotaStats()->registerSizeMapper(mapSize);
  CPPUNIT_ASSERT_NO_THROW(viewMaster->initialize());
  createSubTree(viewMaster, "/", 4, 10, 100);
  //----------------------------------------------------------------------------
  // Modify some stuff
  //----------------------------------------------------------------------------
  modifySubTree(viewMaster, "/dir1");
  deleteAllReplicasRec(viewMaster, "/dir1/dir1/dir1");
  viewMaster->removeContainer("/dir1/dir1/dir1", true);
  //----------------------------------------------------------------------------
  // Run compaction
  //----------------------------------------------------------------------------
  viewMaster->finalize();
  eos::LogCompactingStats stats;
  eos::LogManager::compactLog(fileNameFileMD, fileNameFileMD + "c", stats, 0);
  eos::LogManager::compactLog(fileNameContMD, fileNameContMD + "c", stats, 0);
  unlink(fileNameFileMD.c_str());
  unlink(fileNameContMD.c_str());
  //----------------------------------------------------------------------------
  // Reboot the master
  //----------------------------------------------------------------------------
  eos::FileSystemView* fsViewMaster  = new eos::FileSystemView;
  eos::FileSystemView* fsViewSlave   = new eos::FileSystemView;
  contSettings1["changelog_path"] = fileNameContMD + "c";
  fileSettings1["changelog_path"] = fileNameFileMD + "c";
  fileSvcMaster->configure(fileSettings1);
  contSvcMaster->configure(contSettings1);
  fileSvcMaster->addChangeListener(fsViewMaster);
  viewMaster->getQuotaStats()->registerSizeMapper(mapSize);
  CPPUNIT_ASSERT_NO_THROW(viewMaster->initialize());
  viewMaster->createContainer("/newdir1", true);
  createSubTree(viewMaster, "/newdir1", 2, 10, 100);
  modifySubTree(viewMaster, "/newdir1");
  deleteAllReplicasRec(viewMaster, "/newdir1/dir1");
  viewMaster->removeContainer("/newdir1/dir1", true);
  eos::IContainerMD* contMaster2 = 0;
  eos::IContainerMD* contMaster3 = 0;
  CPPUNIT_ASSERT_NO_THROW(contMaster2 = viewMaster->createContainer("/newdir2",
                                        true));
  CPPUNIT_ASSERT_NO_THROW(contMaster3 = viewMaster->createContainer("/newdir3",
                                        true));
  eos::IQuotaNode* qnMaster2 = 0;
  eos::IQuotaNode* qnMaster3 = 0;
  CPPUNIT_ASSERT_NO_THROW(qnMaster2 = viewMaster->registerQuotaNode(contMaster2));
  CPPUNIT_ASSERT_NO_THROW(qnMaster3 = viewMaster->registerQuotaNode(contMaster3));
  CPPUNIT_ASSERT(qnMaster2);
  CPPUNIT_ASSERT(qnMaster3);
  //----------------------------------------------------------------------------
  // Set up the slave
  //----------------------------------------------------------------------------
  eos::ChangeLogContainerMDSvc* contSvcSlave = new eos::ChangeLogContainerMDSvc;
  eos::ChangeLogFileMDSvc*      fileSvcSlave = new eos::ChangeLogFileMDSvc;
  eos::IView*                   viewSlave    = new eos::HierarchicalView;
  fileSvcSlave->addChangeListener(fsViewSlave);
  fileSvcSlave->setContainerService(contSvcSlave);
  RWLock lock;
  contSvcSlave->setSlaveLock(&lock);
  fileSvcSlave->setSlaveLock(&lock);
  std::map<std::string, std::string> fileSettings2;
  std::map<std::string, std::string> contSettings2;
  std::map<std::string, std::string> settings2;
  contSettings2["changelog_path"]   = fileNameContMD + "c";
  contSettings2["slave_mode"]       = "true";
  contSettings2["poll_interval_us"] = "1000";
  fileSettings2["changelog_path"]   = fileNameFileMD + "c";
  fileSettings2["slave_mode"]       = "true";
  fileSettings2["poll_interval_us"] = "1000";
  contSvcSlave->configure(contSettings2);
  fileSvcSlave->configure(fileSettings2);
  viewSlave->setContainerMDSvc(contSvcSlave);
  viewSlave->setFileMDSvc(fileSvcSlave);
  viewSlave->configure(settings2);
  viewSlave->getQuotaStats()->registerSizeMapper(mapSize);
  fileSvcSlave->setQuotaStats(viewSlave->getQuotaStats());
  contSvcSlave->setQuotaStats(viewSlave->getQuotaStats());
  CPPUNIT_ASSERT_NO_THROW(viewSlave->initialize());
  CPPUNIT_ASSERT_NO_THROW(contSvcSlave->startSlave());
  CPPUNIT_ASSERT_NO_THROW(fileSvcSlave->startSlave());
  contMaster2 = 0;
  contMaster3 = 0;
  CPPUNIT_ASSERT_NO_THROW(contMaster2 = viewMaster->getContainer("/newdir2"));
  CPPUNIT_ASSERT_NO_THROW(contMaster3 = viewMaster->getContainer("/newdir3"));
  qnMaster2 = 0;
  qnMaster3 = 0;
  CPPUNIT_ASSERT_NO_THROW(qnMaster2 = viewMaster->getQuotaNode(contMaster2));
  CPPUNIT_ASSERT_NO_THROW(qnMaster3 = viewMaster->getQuotaNode(contMaster3));
  CPPUNIT_ASSERT(qnMaster2);
  CPPUNIT_ASSERT(qnMaster3);
  CPPUNIT_ASSERT_NO_THROW(viewMaster->createContainer("/newdir4", true));
  CPPUNIT_ASSERT_NO_THROW(viewMaster->createContainer("/newdir5", true));
  CPPUNIT_ASSERT_NO_THROW(createSubTree(viewMaster, "/newdir2", 2, 10, 100));
  CPPUNIT_ASSERT_NO_THROW(modifySubTree(viewMaster, "/newdir2"));
  CPPUNIT_ASSERT_NO_THROW(createSubTree(viewMaster, "/newdir3", 2, 10, 100));
  CPPUNIT_ASSERT_NO_THROW(cleanUpQuotaRec(viewMaster,
                                          viewMaster->getContainer("/newdir2/dir3")));
  deleteAllReplicasRec(viewMaster, "/newdir2/dir3");
  CPPUNIT_ASSERT_NO_THROW(viewMaster->removeContainer("/newdir2/dir3", true));
  CPPUNIT_ASSERT_NO_THROW(modifySubTree(viewMaster, "/newdir3"));
  CPPUNIT_ASSERT_NO_THROW(createSubTree(viewMaster, "/newdir4", 2, 10, 100));
  CPPUNIT_ASSERT_NO_THROW(createSubTree(viewMaster, "/newdir5", 2, 10, 100));
  CPPUNIT_ASSERT_NO_THROW(modifySubTree(viewMaster, "/newdir4"));
  CPPUNIT_ASSERT_NO_THROW(cleanUpQuotaRec(viewMaster,
                                          viewMaster->getContainer("/newdir3/dir1")));
  deleteAllReplicasRec(viewMaster, "/newdir3/dir1");
  CPPUNIT_ASSERT_NO_THROW(viewMaster->removeContainer("/newdir3/dir1", true));
  deleteAllReplicasRec(viewMaster, "/newdir3/dir2");
  unlinkReplicas(viewMaster, viewMaster->getContainer("/newdir1/dir2"));
  unlinkReplicas(viewMaster, viewMaster->getContainer("/newdir4/dir2"));
  unlinkReplicas(viewMaster, viewMaster->getContainer("/newdir5/dir1"));
  unlinkReplicas(viewMaster, viewMaster->getContainer("/newdir5/dir2"));
  //----------------------------------------------------------------------------
  // Move some files around and rename them
  //----------------------------------------------------------------------------
  eos::IContainerMD* parent1     = 0;
  eos::IContainerMD* parent2     = 0;
  eos::IFileMD*      toBeMoved   = 0;
  eos::IFileMD*      toBeRenamed = 0;
  CPPUNIT_ASSERT_NO_THROW(parent1 = viewMaster->createContainer("/dest", true));
  CPPUNIT_ASSERT_NO_THROW(parent2 = viewMaster->getContainer("/dir0/dir0"));
  CPPUNIT_ASSERT_NO_THROW(toBeMoved = viewMaster->getFile("/dir0/dir0/file0"));
  CPPUNIT_ASSERT_NO_THROW(toBeRenamed = viewMaster->getFile("/dir0/dir0/file1"));
  CPPUNIT_ASSERT(parent1);
  CPPUNIT_ASSERT(parent2);
  CPPUNIT_ASSERT(toBeMoved);
  CPPUNIT_ASSERT(toBeRenamed);
  parent2->removeFile(toBeMoved->getName());
  parent1->addFile(toBeMoved);
  viewMaster->updateFileStore(toBeMoved);
  viewMaster->renameFile(toBeRenamed, "file0");
  //----------------------------------------------------------------------------
  // Check
  //----------------------------------------------------------------------------
  sleep(5);
  lock.readLock();
  compareTrees(viewMaster, viewSlave,
               viewMaster->getContainer("/"),
               viewSlave->getContainer("/"));
  compareFileSystems(fsViewMaster, fsViewSlave);
  eos::IQuotaNode* qnSlave2 = 0;
  eos::IQuotaNode* qnSlave3 = 0;
  eos::IContainerMD* contSlave2 = viewSlave->getContainer("/newdir2");
  eos::IContainerMD* contSlave3 = viewSlave->getContainer("/newdir3");
  CPPUNIT_ASSERT(contSlave2);
  CPPUNIT_ASSERT(contSlave3);
  CPPUNIT_ASSERT_NO_THROW(qnSlave2 = viewSlave->getQuotaNode(contSlave2));
  CPPUNIT_ASSERT_NO_THROW(qnSlave3 = viewSlave->getQuotaNode(contSlave3));
  CPPUNIT_ASSERT(qnSlave2);
  CPPUNIT_ASSERT(qnSlave3);
  CPPUNIT_ASSERT(qnSlave2 != qnMaster2);
  CPPUNIT_ASSERT(qnSlave3 != qnMaster3);
  eos::IQuotaNode* qnS[2];
  qnS[0] = qnSlave2;
  qnS[1] = qnSlave3;
  eos::IQuotaNode* qnM[2];
  qnM[0] = qnMaster2;
  qnM[1] = qnMaster3;

  for (int i = 0; i < 2; ++i)
  {
    eos::IQuotaNode* qnSlave  = qnS[i];
    eos::IQuotaNode* qnMaster = qnM[i];
    CPPUNIT_ASSERT(qnSlave->getPhysicalSpaceByUser(0)  ==
                   qnMaster->getPhysicalSpaceByUser(0));
    CPPUNIT_ASSERT(qnSlave->getUsedSpaceByUser(0)      ==
                   qnMaster->getUsedSpaceByUser(0));
    CPPUNIT_ASSERT(qnSlave->getPhysicalSpaceByGroup(0) ==
                   qnMaster->getPhysicalSpaceByGroup(0));
    CPPUNIT_ASSERT(qnSlave->getUsedSpaceByGroup(0)     ==
                   qnMaster->getUsedSpaceByGroup(0));
    CPPUNIT_ASSERT(qnSlave->getNumFilesByUser(0)       ==
                   qnMaster->getNumFilesByUser(0));
    CPPUNIT_ASSERT(qnSlave->getNumFilesByGroup(0)      ==
                   qnMaster->getNumFilesByGroup(0));
  }

  lock.unLock();
  //----------------------------------------------------------------------------
  // Clean up
  //----------------------------------------------------------------------------
  CPPUNIT_ASSERT_NO_THROW(contSvcSlave->stopSlave());
  CPPUNIT_ASSERT_NO_THROW(fileSvcSlave->stopSlave());
  viewSlave->finalize();
  viewMaster->finalize();
  delete viewSlave;
  delete viewMaster;
  delete contSvcSlave;
  delete contSvcMaster;
  delete fileSvcSlave;
  delete fileSvcMaster;
  delete fsViewMaster;
  delete fsViewSlave;
  unlink(fileNameFileMD.c_str());
  unlink(fileNameContMD.c_str());
  unlink((fileNameFileMD + "c").c_str());
  unlink((fileNameContMD + "c").c_str());
}
