/*
 * SessionPackrat.cpp
 *
 * Copyright (C) 2009-14 by RStudio, Inc.
 *
 * Unless you have received this program directly from RStudio pursuant
 * to the terms of a commercial license agreement with RStudio, then
 * this program is licensed to you under the terms of version 3 of the
 * GNU Affero General Public License. This program is distributed WITHOUT
 * ANY EXPRESS OR IMPLIED WARRANTY, INCLUDING THOSE OF NON-INFRINGEMENT,
 * MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE. Please refer to the
 * AGPL (http://www.gnu.org/licenses/agpl-3.0.txt) for more details.
 *
 */

#include "SessionPackrat.hpp"

#include <core/Exec.hpp>
#include <core/FileSerializer.hpp>
#include <core/Hash.hpp>
#include <core/system/FileMonitor.hpp>
#include <core/RecursionGuard.hpp>

#include <r/RExec.hpp>
#include <r/RJson.hpp>
#include <r/session/RClientState.hpp>
#include <r/RRoutines.hpp>
#include <r/ROptions.hpp>

#include <session/projects/SessionProjects.hpp>
#include <session/SessionAsyncRProcess.hpp>
#include <session/SessionModuleContext.hpp>
#include <session/SessionUserSettings.hpp>

#include "SessionPackages.hpp"
#include "session-config.h"

using namespace core;

#ifdef TRACE_PACKRAT_OUTPUT
#define PACKRAT_TRACE(x) \
   std::cerr << "(packrat) " << x << std::endl;
#else
#define PACKRAT_TRACE(x) 
#endif

#define kPackratFolder "packrat/"
#define kPackratLockfile "packrat.lock"
#define kPackratLibPath kPackratFolder "lib"
#define kPackratLockfilePath kPackratFolder kPackratLockfile

#define kPackratActionRestore "restore"
#define kPackratActionClean "clean"
#define kPackratActionSnapshot "snapshot"

namespace session {

namespace {

bool isRequiredPackratInstalled()
{
   return module_context::isPackageVersionInstalled("packrat", "0.2.0.109");
}

} // anonymous namespace

namespace modules { 
namespace packrat {

namespace {

// Current Packrat actions and state -----------------------------------------

enum PackratActionType 
{
   PACKRAT_ACTION_NONE = 0,
   PACKRAT_ACTION_SNAPSHOT = 1,
   PACKRAT_ACTION_RESTORE = 2,
   PACKRAT_ACTION_CLEAN = 3,
   PACKRAT_ACTION_UNKNOWN = 4 
};

enum PackratHashType
{
   HASH_TYPE_LOCKFILE = 0,
   HASH_TYPE_LIBRARY = 1
};

// Hash states are used for two purposes:
// 1) To ascertain whether an object has undergone a meaningful change--for
//    instance, if the library state is different after an operation
// 2) To track the last-resolved state of an object, as an aid for discovering
//    what actions are appropriate on the object
//
// As an example, take the lockfile hash:
// HASH_STATE_COMPUTED != HASH_STATE_OBSERVED
//    The client's view reflects a different lockfile state. Refresh the client
//    view.
// HASH_STATE_OBSERVED != HASH_STATE_RESOLVED
//    The content in the lockfile has changed since the last time a snapshot or
//    restore was performed. The user should perform a 'restore'.
// HASH_STATE_COMPUTED == HASH_STATE_RESOLVED
//    The content of the lockfile is up-to-date and no action is needed.
//
enum PackratHashState
{
   HASH_STATE_RESOLVED = 0,  // The state last known to be consistent (stored)
   HASH_STATE_OBSERVED = 1,  // The state last viewed by the client (stored)
   HASH_STATE_COMPUTED = 2   // The current state (not stored)
};

enum PendingSnapshotAction
{
   SET_PENDING_SNAPSHOT = 0,
   COMPLETE_SNAPSHOT = 1
};

PackratActionType packratAction(const std::string& str)
{
   if (str == kPackratActionSnapshot) 
      return PACKRAT_ACTION_SNAPSHOT;
   else if (str == kPackratActionRestore)
      return PACKRAT_ACTION_RESTORE;
   else if (str == kPackratActionClean)
      return PACKRAT_ACTION_CLEAN;
   else 
      return PACKRAT_ACTION_UNKNOWN;
}

std::string packratActionName(PackratActionType action)
{
   switch (action) {
      case PACKRAT_ACTION_SNAPSHOT:
         return kPackratActionSnapshot;
         break;
      case PACKRAT_ACTION_RESTORE: 
         return kPackratActionRestore;
         break;
      case PACKRAT_ACTION_CLEAN:
         return kPackratActionClean;
         break;
      default:
         return "";
   }
}

static PackratActionType s_runningPackratAction = PACKRAT_ACTION_NONE;

// Forward declarations ------------------------------------------------------

void performAutoSnapshot(const std::string& targetHash);
void pendingSnapshot(PendingSnapshotAction action);
bool getPendingActions(PackratActionType action, json::Value* pActions);
void emitPackagesChanged();
void resolveStateAfterAction(PackratActionType action, 
                             PackratHashType hashType);
std::string computeLockfileHash();
std::string computeLibraryHash();

// Library and lockfile hashing and comparison -------------------------------

// Returns the storage key for the given hash type and state
std::string keyOfHashType(PackratHashType hashType, PackratHashState hashState)
{
   std::string hashKey  = "packrat";
   hashKey.append(hashType == HASH_TYPE_LOCKFILE ? "Lockfile" : "Library");
   hashKey.append(hashState == HASH_STATE_OBSERVED ? "Observed" : "Resolved");
   return hashKey;
}

// Given the hash type and state, return the hash
std::string getHash(PackratHashType hashType, PackratHashState hashState)
{
   // For computed hashes, do the computation
   if (hashState == HASH_STATE_COMPUTED)
   {
      if (hashType == HASH_TYPE_LOCKFILE)
         return computeLockfileHash();
      else
         return computeLibraryHash();
   }
   // For stored hashes, look up in project persistent storage
   json::Value hash = 
      r::session::clientState().getProjectPersistent(
            "packrat",
            keyOfHashType(hashType, hashState));
   if (hash.type() == json::StringType) 
      return hash.get_str();
   else
      return "";
}

std::string updateHash(PackratHashType hashType, PackratHashState hashState)
{
   std::string newHash = getHash(hashType, HASH_STATE_COMPUTED);
   std::string oldHash = getHash(hashType, hashState);
   if (newHash != oldHash)
   {
      PACKRAT_TRACE("updating " << keyOfHashType(hashType, hashState) << 
                    " (" << oldHash << " -> " << newHash << ")");
      r::session::clientState().putProjectPersistent(
            "packrat", 
            keyOfHashType(hashType, hashState), 
            newHash);
   }
   return newHash;
}

// adds content from the given file to the given file if it's a 
// DESCRIPTION file (used to summarize library content for hashing)
bool addDescContent(int level, const FilePath& path, std::string* pDescContent)
{
   std::string newDescContent;
   if (path.filename() == "DESCRIPTION") 
   {
      Error error = readStringFromFile(path, &newDescContent);
      pDescContent->append(newDescContent);
   }
   return true;
}

// computes a hash of the content of all DESCRIPTION files in the Packrat
// private library
std::string computeLibraryHash()
{
   FilePath libraryPath = 
      projects::projectContext().directory().complete(kPackratLibPath);

   // find all DESCRIPTION files in the library and concatenate them to form
   // a hashable state
   std::string descFileContent;
   libraryPath.childrenRecursive(
         boost::bind(addDescContent, _1, _2, &descFileContent));

   if (descFileContent.empty())
      return "";

   return hash::crc32HexHash(descFileContent);
}

// computes the hash of the current project's lockfile
std::string computeLockfileHash()
{
   FilePath lockFilePath = 
      projects::projectContext().directory().complete(kPackratLockfilePath);

   if (!lockFilePath.exists()) 
      return "";

   std::string lockFileContent;
   Error error = readStringFromFile(lockFilePath, &lockFileContent);
   if (error)
   {
      LOG_ERROR(error);
      return "";
   }
   
   return hash::crc32HexHash(lockFileContent);
}

void checkHashes(
      PackratHashType hashType, 
      PackratHashState hashState,
      boost::function<void(const std::string&, const std::string&)> onMismatch)
{
   // if a request to check hashes comes in while we're already checking hashes,
   // drop it: it's very likely that the file monitor has discovered a change
   // to a file we've already hashed.
   DROP_RECURSIVE_CALLS;

   std::string oldHash = getHash(hashType, hashState);
   std::string newHash = getHash(hashType, HASH_STATE_COMPUTED);

   // hashes match, no work needed
   if (oldHash == newHash)
      return;
   else 
      onMismatch(oldHash, newHash);
}

bool isHashUnresolved(PackratHashType hashType)
{
   std::string observedHash = getHash(hashType, HASH_STATE_OBSERVED);
   std::string resolvedHash = getHash(hashType, HASH_STATE_RESOLVED);
   if (observedHash.empty() || resolvedHash.empty())
      return false;
   return observedHash != resolvedHash;
}

// Auto-snapshot -------------------------------------------------------------

class AutoSnapshot: public async_r::AsyncRProcess
{
public:
   static boost::shared_ptr<AutoSnapshot> create(
         const FilePath& projectDir, 
         const std::string& targetHash)
   {
      boost::shared_ptr<AutoSnapshot> pSnapshot(new AutoSnapshot());
      std::string snapshotCmd;
      Error error = r::exec::RFunction(
            ".rs.getAutoSnapshotCmd",
            projectDir.absolutePath()).call(&snapshotCmd);
      if (error)
         LOG_ERROR(error); // will also be reported in the console

      PACKRAT_TRACE("starting auto snapshot, R command: " << snapshotCmd);
      pSnapshot->setTargetHash(targetHash);
      pSnapshot->start(snapshotCmd.c_str(), projectDir);
      return pSnapshot;
   }

   std::string getTargetHash()
   {
      return targetHash_;
   }
  
private:
   void setTargetHash(const std::string& targetHash)
   {
      targetHash_ = targetHash;
   }

   void onStderr(const std::string& output)
   {
      PACKRAT_TRACE("(auto snapshot) " << output);
   }

   void onStdout(const std::string& output)
   {
      PACKRAT_TRACE("(auto snapshot) " << output);
   }
   
   void onCompleted(int exitStatus)
   {
      PACKRAT_TRACE("finished auto snapshot, exit status = " << exitStatus);
      if (exitStatus != 0)
         return;
      pendingSnapshot(COMPLETE_SNAPSHOT);
   }

   std::string targetHash_;
};

void pendingSnapshot(PendingSnapshotAction action)
{
   static int pendingSnapshots = 0;
   if (action == SET_PENDING_SNAPSHOT)
   {
      pendingSnapshots++;
      PACKRAT_TRACE("snapshot requested while running, queueing ("
                    << pendingSnapshots << ")");
      return;
   }
   else if (action == COMPLETE_SNAPSHOT)
   {
      if (pendingSnapshots > 0)
      {
         PACKRAT_TRACE("executing pending snapshot");
         pendingSnapshots = 0;
         performAutoSnapshot(computeLibraryHash());
      }
      else
      {
         resolveStateAfterAction(PACKRAT_ACTION_SNAPSHOT, HASH_TYPE_LIBRARY);
      }
   }
}

void performAutoSnapshot(const std::string& newHash)
{
   static boost::shared_ptr<AutoSnapshot> pAutoSnapshot;
   if (pAutoSnapshot && 
       pAutoSnapshot->isRunning())
   {
      // is the requested snapshot for the same state we're already 
      // snapshotting? if it is, ignore the request
      if (pAutoSnapshot->getTargetHash() == newHash)
      {
         PACKRAT_TRACE("snapshot already running (" << newHash << ")");
         return;
      }
      else
      {
         pendingSnapshot(SET_PENDING_SNAPSHOT);
         return;
      }
   }

   // start a new auto-snapshot
   pAutoSnapshot = AutoSnapshot::create(
         projects::projectContext().directory(),
         newHash);
}

// Library and lockfile monitoring -------------------------------------------

// indicates whether there are any actions that would be performed if the given
// action were executed; if there are actions, they are returned in pActions
bool getPendingActions(PackratActionType action, json::Value* pActions)
{
   // get the list of actions from Packrat
   SEXP actions;
   r::sexp::Protect protect;
   Error error = r::exec::RFunction(".rs.pendingActions", 
         packratActionName(action), 
         projects::projectContext().directory().absolutePath())
         .call(&actions, &protect);

   // return nothing if an error occurs or there are no actions
   if (error)
   {
      LOG_ERROR(error);
      return false;
   }
   if (r::sexp::length(actions) == 0)
      return false;

   // convert the action list to JSON if needed
   if (pActions) 
      error = r::json::jsonValueFromObject(actions, pActions);

   return !error;
}

void onLockfileUpdate(const std::string& oldHash, const std::string& newHash)
{
   // if the lockfile changed, refresh to show the new Packrat state 
   emitPackagesChanged();
}

void onLibraryUpdate(const std::string& oldHash, const std::string& newHash)
{
   // perform an auto-snapshot if we don't have a pending restore
   if (!isHashUnresolved(HASH_TYPE_LOCKFILE)) 
      performAutoSnapshot(newHash);
   else 
   {
      PACKRAT_TRACE("lockfile observed hash " << 
                    getHash(HASH_TYPE_LOCKFILE, HASH_STATE_OBSERVED) << 
                    " doesn't match resolved hash " <<
                    getHash(HASH_TYPE_LOCKFILE, HASH_STATE_RESOLVED) <<
                    ", skipping auto snapshot"); 
      emitPackagesChanged();
   }
}

void onFileChanged(FilePath sourceFilePath)
{
   // ignore file changes while Packrat is running
   if (s_runningPackratAction != PACKRAT_ACTION_NONE)
      return;
   
   // we only care about mutations to files in the Packrat library directory
   // (and packrat.lock)
   FilePath libraryPath = 
      projects::projectContext().directory().complete(kPackratLibPath);

   if (sourceFilePath.filename() == kPackratLockfile)
   {
      PACKRAT_TRACE("detected change to lockfile " << sourceFilePath);
      checkHashes(HASH_TYPE_LOCKFILE, HASH_STATE_OBSERVED, onLockfileUpdate);
   }
   else if (sourceFilePath.isWithin(libraryPath) && 
            (sourceFilePath.isDirectory() || 
             sourceFilePath.filename() == "DESCRIPTION"))
   {
      // ignore changes in the RStudio-managed manipulate and rstudio 
      // directories and the files within them
      if (sourceFilePath.filename() == "manipulate" ||
          sourceFilePath.filename() == "rstudio" ||
          sourceFilePath.parent().filename() == "manipulate" || 
          sourceFilePath.parent().filename() == "rstudio")
      {
         return;
      }
      PACKRAT_TRACE("detected change to library file " << sourceFilePath);
      checkHashes(HASH_TYPE_LIBRARY, HASH_STATE_OBSERVED, onLibraryUpdate);
   }
}

void onFilesChanged(const std::vector<core::system::FileChangeEvent>& changes)
{
   BOOST_FOREACH(const core::system::FileChangeEvent& fileChange, changes)
   {
      FilePath changedFilePath(fileChange.fileInfo().absolutePath());
      onFileChanged(changedFilePath);
   }
}

void emitPackagesChanged()
{
   ClientEvent event(client_events::kInstalledPackagesChanged);
   module_context::enqueClientEvent(event);
}

// RPC -----------------------------------------------------------------------

Error installPackrat(const json::JsonRpcRequest& request,
                    json::JsonRpcResponse* pResponse)
{
   Error error = module_context::installEmbeddedPackage("packrat");
   if (error)
   {
      std::string desc = error.getProperty("description");
      if (desc.empty())
         desc = error.summary();

      module_context::consoleWriteError(desc + "\n");

      LOG_ERROR(error);
   }

   pResponse->setResult(!error);

   return Success();
}

Error getPackratPrerequisites(const json::JsonRpcRequest& request,
                              json::JsonRpcResponse* pResponse)
{
   json::Object prereqJson;
   prereqJson["build_tools_available"] = module_context::canBuildCpp();
   prereqJson["package_available"] = isRequiredPackratInstalled();
   pResponse->setResult(prereqJson);
   return Success();
}


Error getPackratContext(const json::JsonRpcRequest& request,
                        json::JsonRpcResponse* pResponse)
{
   pResponse->setResult(module_context::packratContextAsJson());
   return Success();
}

Error packratBootstrap(const json::JsonRpcRequest& request,
                       json::JsonRpcResponse* pResponse)
{
   // read params
   std::string dir;
   bool enter = false;
   Error error = json::readParams(request.params, &dir, &enter);
   if (error)
      return error;

   // convert to file path then to system encoding
   FilePath dirPath = module_context::resolveAliasedPath(dir);
   dir = string_utils::utf8ToSystem(dirPath.absolutePath());

   // bootstrap
   r::exec::RFunction bootstrap("packrat:::bootstrap");
   bootstrap.addParam("project", dir);
   bootstrap.addParam("enter", enter);
   bootstrap.addParam("restart", false);

   error = bootstrap.call();
   if (error)
      LOG_ERROR(error); // will also be reported in the console

   // return status
   return Success();
}

Error initPackratMonitoring()
{
   FilePath lockfilePath = 
      projects::projectContext().directory().complete(kPackratLockfilePath);

   // if there's no lockfile, presume that this isn't a Packrat project
   if (!lockfilePath.exists())
      return Success();

   // listen for changes to the project files 
   PACKRAT_TRACE("found " << lockfilePath.absolutePath() << 
                 ", init monitoring");

   session::projects::FileMonitorCallbacks cb;
   cb.onFilesChanged = onFilesChanged;
   projects::projectContext().subscribeToFileMonitor("Packrat", cb);
   module_context::events().onSourceEditorFileSaved.connect(onFileChanged);

   return Success();
}

// runs after an (auto) snapshot or restore
void resolveStateAfterAction(PackratActionType action, 
                             PackratHashType hashType)
{
   // if the action changed the underlying store, tell the client to refresh
   // its view
   if (getHash(hashType, HASH_STATE_OBSERVED) != 
         getHash(hashType, HASH_STATE_COMPUTED))
   {
      emitPackagesChanged();
   }

   // if the action moved us to a consistent state, mark the state as 
   // resolved
   if (!getPendingActions(action, NULL))
   {
      updateHash(HASH_TYPE_LIBRARY, HASH_STATE_RESOLVED);
      updateHash(HASH_TYPE_LOCKFILE, HASH_STATE_RESOLVED);
   }
}

// Notification that a packrat action has either started or
// stopped (indicated by the "running" flag). Possible values for
// action are: "snapshot", "restore", and "clean"
void onPackratAction(const std::string& project,
                     const std::string& action,
                     bool running)
{
   // if this doesn't apply to the current project then skip it
   if (!core::system::realPathsEqual(
          projects::projectContext().directory(), FilePath(project)))
   {
      return;
   }

   if (running && (s_runningPackratAction != PACKRAT_ACTION_NONE))
      PACKRAT_TRACE("warning: '" << action << "' executed while action " << 
                    s_runningPackratAction << " was already running");

   PACKRAT_TRACE("packrat action '" << action << "' " <<
                 (running ? "started" : "finished"));
   // action started, cache it and return
   if (running) 
   {
      s_runningPackratAction = packratAction(action);
      return;
   }

   PackratActionType completedAction = s_runningPackratAction;
   s_runningPackratAction = PACKRAT_ACTION_NONE;

   // action ended, update hashes accordingly
   switch (completedAction)
   {
      case PACKRAT_ACTION_RESTORE:
         resolveStateAfterAction(PACKRAT_ACTION_RESTORE, HASH_TYPE_LOCKFILE);
         break;
      case PACKRAT_ACTION_SNAPSHOT:
         resolveStateAfterAction(PACKRAT_ACTION_SNAPSHOT, HASH_TYPE_LIBRARY);
         break;
      default:
         break;
   }
}

SEXP rs_onPackratAction(SEXP projectSEXP, SEXP actionSEXP, SEXP runningSEXP)
{
   std::string project = r::sexp::safeAsString(projectSEXP);
   std::string action = r::sexp::safeAsString(actionSEXP);
   bool running = r::sexp::asLogical(runningSEXP);

   onPackratAction(project, action, running);

   return R_NilValue;
}


void detectReposChanges()
{
   static SEXP s_lastReposSEXP = R_UnboundValue;
   SEXP reposSEXP = r::options::getOption("repos");
   if (s_lastReposSEXP == R_UnboundValue)
   {
      s_lastReposSEXP = reposSEXP;
   }
   else if (reposSEXP != s_lastReposSEXP)
   {
      s_lastReposSEXP = reposSEXP;

      // TODO: ensure that a snapshot takes place

   }
}

void onDetectChanges(module_context::ChangeSource source)
{
   if (source == module_context::ChangeSourceREPL)
      detectReposChanges();
}

void onDeferredInit(bool)
{
   // additional stuff if we are in packrat mode
   if (module_context::packratContext().modeOn)
   {
      Error error = r::exec::RFunction(".rs.installPackratActionHook").call();
      if (error)
         LOG_ERROR(error);

      error = initPackratMonitoring();
      if (error)
         LOG_ERROR(error);

      module_context::events().onDetectChanges.connect(onDetectChanges);
   }
}

} // anonymous namespace

json::Object contextAsJson(const module_context::PackratContext& context)
{
   json::Object contextJson;
   contextJson["available"] = context.available;
   contextJson["applicable"] = context.applicable;
   contextJson["packified"] = context.packified;
   contextJson["mode_on"] = context.modeOn;
   return contextJson;
}

json::Object contextAsJson()
{
   module_context::PackratContext context = module_context::packratContext();
   return contextAsJson(context);
}

void annotatePendingActions(json::Object *pJson)
{
   json::Value restoreActions;
   json::Value snapshotActions;
   json::Value cleanActions;
   json::Object& json = *pJson;

   // compute new hashes and mark them observed
   std::string libraryHash = 
      updateHash(HASH_TYPE_LIBRARY, HASH_STATE_OBSERVED);
   std::string lockfileHash = 
      updateHash(HASH_TYPE_LOCKFILE, HASH_STATE_OBSERVED);

   // check for resolved states
   bool libraryDirty = 
      libraryHash != getHash(HASH_TYPE_LIBRARY, HASH_STATE_RESOLVED);
   bool lockfileDirty = 
      lockfileHash != getHash(HASH_TYPE_LOCKFILE, HASH_STATE_RESOLVED);

   if (libraryDirty)
      getPendingActions(PACKRAT_ACTION_SNAPSHOT, &snapshotActions);
   if (lockfileDirty)
      getPendingActions(PACKRAT_ACTION_RESTORE, &restoreActions);

   getPendingActions(PACKRAT_ACTION_CLEAN, &cleanActions);
      
   json["restore_actions"] = restoreActions;
   json["snapshot_actions"] = snapshotActions;
   json["clean_actions"] = cleanActions;
}

Error initialize()
{
   // register deferred init (since we need to call into the packrat package
   // we need to wait until all other modules initialize and all R routines
   // are initialized -- otherwise the package load hook attempts to call
   // rs_packageLoaded and can't find it
   module_context::events().onDeferredInit.connect(onDeferredInit);

   // register packrat action hook
   R_CallMethodDef onPackratActionMethodDef ;
   onPackratActionMethodDef.name = "rs_onPackratAction" ;
   onPackratActionMethodDef.fun = (DL_FUNC) rs_onPackratAction ;
   onPackratActionMethodDef.numArgs = 3;
   r::routines::addCallMethod(onPackratActionMethodDef);

   using boost::bind;
   using namespace module_context;
   ExecBlock initBlock;
   initBlock.addFunctions()
      (bind(registerRpcMethod, "install_packrat", installPackrat))
      (bind(registerRpcMethod, "get_packrat_prerequisites", getPackratPrerequisites))
      (bind(registerRpcMethod, "get_packrat_context", getPackratContext))
      (bind(registerRpcMethod, "packrat_bootstrap", packratBootstrap))
      (bind(sourceModuleRFile, "SessionPackrat.R"));
   return initBlock.execute();
}

} // namespace packrat
} // namespace modules

namespace module_context {

PackratContext packratContext()
{
   PackratContext context;

   // NOTE: when we switch to auto-installing packrat we need to update
   // this check to look for R >= whatever packrat requires (we don't
   // need to look for R >= 3.0 as we do for rmarkdown/shiny because
   // build tools will be installed prior to attempting to auto-install
   // the embedded version of packrat

   context.available = isRequiredPackratInstalled();

   context.applicable = context.available &&
                        projects::projectContext().hasProject();

   if (context.applicable)
   {
      FilePath projectDir = projects::projectContext().directory();
      std::string projectPath =
         string_utils::utf8ToSystem(projectDir.absolutePath());
      Error error = r::exec::RFunction(
                           "packrat:::checkPackified",
                           /* project = */ projectPath,
                           /* silent = */ true).call(&context.packified);
      if (error)
         LOG_ERROR(error);

      if (context.packified)
      {
         error = r::exec::RFunction(
                            ".rs.isPackratModeOn",
                            projectPath).call(&context.modeOn);
         if (error)
            LOG_ERROR(error);
      }
   }

   return context;
}


json::Object packratContextAsJson()
{
   return modules::packrat::contextAsJson();
}

namespace {

void copyOption(SEXP optionsSEXP, const std::string& listName,
                json::Object* pOptionsJson, const std::string& jsonName,
                bool defaultValue)
{
   bool value = defaultValue;
   Error error = r::sexp::getNamedListElement(optionsSEXP,
                                              listName,
                                              &value,
                                              defaultValue);
   if (error)
   {
      error.addProperty("option", listName);
      LOG_ERROR(error);
   }

   (*pOptionsJson)[jsonName] = value;
}

json::Object defaultPackratOptions()
{
   json::Object optionsJson;
   optionsJson["auto_snapshot"] = true;
   optionsJson["vcs_ignore_lib"] = true;
   optionsJson["vcs_ignore_src"] = false;
   return optionsJson;
}

} // anonymous namespace

json::Object packratOptionsAsJson()
{
   PackratContext context = packratContext();
   if (context.packified)
   {
      // create options to return
      json::Object optionsJson;

      // get the options from packrat
      FilePath projectDir = projects::projectContext().directory();
      r::exec::RFunction getOpts("packrat:::get_opts");
      getOpts.addParam("simplify", false);
      getOpts.addParam("project", module_context::createAliasedPath(
                                                            projectDir));
      r::sexp::Protect rProtect;
      SEXP optionsSEXP;
      Error error = getOpts.call(&optionsSEXP, &rProtect);
      if (error)
      {
         LOG_ERROR(error);
         return defaultPackratOptions();
      }

      // copy the options into json
      copyOption(optionsSEXP, "auto.snapshot",
                 &optionsJson, "auto_snapshot", true);

      copyOption(optionsSEXP, "vcs.ignore.lib",
                 &optionsJson, "vcs_ignore_lib", true);

      copyOption(optionsSEXP, "vcs.ignore.src",
                 &optionsJson, "vcs_ignore_src", false);

      return optionsJson;
   }
   else
   {
      return defaultPackratOptions();
   }
}



} // namespace module_context
} // namespace session

