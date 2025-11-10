/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "MediaControlService.h"

#ifdef XP_WIN
#  include <process.h>
#  ifndef getpid
#    define getpid _getpid
#  endif
#else
#  include <unistd.h>
#endif

#include "MediaControlUtils.h"
#include "MediaController.h"
#include "mozilla/AppShutdown.h"
#include "mozilla/Assertions.h"
#include "mozilla/Atomics.h"
#include "mozilla/Logging.h"
#include "mozilla/Services.h"
#include "mozilla/StaticPrefs_media.h"
#include "mozilla/StaticPtr.h"
#include "mozilla/intl/Localization.h"
#include "nsIObserverService.h"
#include "nsXULAppAPI.h"

using mozilla::intl::Localization;

#undef LOG
#define LOG(msg, ...)                        \
  MOZ_LOG(gMediaControlLog, LogLevel::Debug, \
          ("MediaControlService=%p, " msg, this, ##__VA_ARGS__))

#undef LOG_MAINCONTROLLER
#define LOG_MAINCONTROLLER(msg, ...) \
  MOZ_LOG(gMediaControlLog, LogLevel::Debug, (msg, ##__VA_ARGS__))

#undef LOG_MAINCONTROLLER_INFO
#define LOG_MAINCONTROLLER_INFO(msg, ...) \
  MOZ_LOG(gMediaControlLog, LogLevel::Info, (msg, ##__VA_ARGS__))

namespace mozilla::dom {

const LinkedList<RefPtr<MediaController>>&
MediaControlService::ControllerManager::GetControllers() const {
  return mControllers;
}

MediaController* MediaControlService::GetControllerByTabId(
    uint64_t tabId) const {
  MOZ_DIAGNOSTIC_ASSERT(mControllerManager);
  return mControllerManager->GetControllerById(tabId);
}

StaticRefPtr<MediaControlService> gMediaControlService;

/* static */
RefPtr<MediaControlService> MediaControlService::GetService() {
  MOZ_DIAGNOSTIC_ASSERT(XRE_IsParentProcess(),
                        "MediaControlService only runs on Chrome process!");
  if (AppShutdown::IsInOrBeyond(ShutdownPhase::XPCOMShutdown)) {
    return nullptr;
  }
  if (!gMediaControlService) {
    gMediaControlService = new MediaControlService();
    gMediaControlService->Init();
  }
  RefPtr<MediaControlService> service = gMediaControlService.get();
  return service;
}

/* static */
void MediaControlService::GenerateMediaControlKey(const GlobalObject& global,
                                                  MediaControlKey aKey,
                                                  double aSeekTime) {
  RefPtr<MediaControlService> service = MediaControlService::GetService();
  if (service) {
    service->GenerateTestMediaControlKey(aKey, aSeekTime);
  }
}

/* static */
void MediaControlService::GetCurrentActiveMediaMetadata(
    const GlobalObject& aGlobal, MediaMetadataInit& aMetadata) {
  if (RefPtr<MediaControlService> service = MediaControlService::GetService()) {
    MediaMetadataBase metadata = service->GetMainControllerMediaMetadata();
    aMetadata.mTitle = metadata.mTitle;
    aMetadata.mArtist = metadata.mArtist;
    aMetadata.mAlbum = metadata.mAlbum;
    for (const auto& artwork : metadata.mArtwork) {
      // If OOM happens resulting in not able to append the element, then we
      // would get incorrect result and fail on test, so we don't need to throw
      // an error explicitly.
      if (MediaImage* image = aMetadata.mArtwork.AppendElement(fallible)) {
        image->mSrc = artwork.mSrc;
        image->mSizes = artwork.mSizes;
        image->mType = artwork.mType;
      }
    }
  }
}

/* static */
MediaSessionPlaybackState
MediaControlService::GetCurrentMediaSessionPlaybackState(
    GlobalObject& aGlobal) {
  if (RefPtr<MediaControlService> service = MediaControlService::GetService()) {
    return service->GetMainControllerPlaybackState();
  }
  return MediaSessionPlaybackState::None;
}

NS_INTERFACE_MAP_BEGIN(MediaControlService)
  NS_INTERFACE_MAP_ENTRY_AMBIGUOUS(nsISupports, nsIObserver)
  NS_INTERFACE_MAP_ENTRY(nsIObserver)
NS_INTERFACE_MAP_END

NS_IMPL_ADDREF(MediaControlService)
NS_IMPL_RELEASE(MediaControlService)

MediaControlService::MediaControlService() {
  LOG("create media control service");
  RefPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
  if (obs) {
    obs->AddObserver(this, "xpcom-shutdown", false);
  }
}

void MediaControlService::Init() {
  mMediaKeysHandler = new MediaControlKeyHandler();
  mControllerManager = MakeUnique<ControllerManager>(this);

  // Initialize the fallback title
  nsTArray<nsCString> resIds{
      "branding/brand.ftl"_ns,
      "dom/media.ftl"_ns,
  };
  RefPtr<Localization> l10n = Localization::Create(resIds, true);
  {
    nsAutoCString translation;
    IgnoredErrorResult rv;
    l10n->FormatValueSync("mediastatus-fallback-title"_ns, {}, translation, rv);
    if (!rv.Failed()) {
      mFallbackTitle = NS_ConvertUTF8toUTF16(translation);
    }
  }
}

MediaControlService::~MediaControlService() {
  LOG("destroy media control service");
  Shutdown();
}

NS_IMETHODIMP
MediaControlService::Observe(nsISupports* aSubject, const char* aTopic,
                             const char16_t* aData) {
  if (!strcmp(aTopic, "xpcom-shutdown")) {
    LOG("XPCOM shutdown");
    MOZ_ASSERT(gMediaControlService);
    RefPtr<nsIObserverService> obs = mozilla::services::GetObserverService();
    if (obs) {
      obs->RemoveObserver(this, "xpcom-shutdown");
    }
    Shutdown();
    gMediaControlService = nullptr;
  }
  return NS_OK;
}

void MediaControlService::Shutdown() {
  for (auto& entry : mTabManagers) {
    if (entry.second) {
      entry.second->Close();
    }
  }
  mTabManagers.clear();
  mControllerManager->Shutdown();
}

bool MediaControlService::RegisterActiveMediaController(
    MediaController* aController) {
  MOZ_DIAGNOSTIC_ASSERT(mControllerManager,
                        "Register controller before initializing service");
  if (!mControllerManager->AddController(aController)) {
    LOG("Fail to register controller %" PRId64, aController->Id());
    return false;
  }
  // Per-tab MPRIS: create a manager/service for this tab if not present
  uint64_t tabId = aController->Id();
  if (!mTabManagers.count(tabId)) {
    static mozilla::Atomic<uint32_t> sInstanceId(0);
    if (sInstanceId == 0) {
      sInstanceId = static_cast<uint32_t>(getpid());
    }
    RefPtr<MediaControlKeyManager> manager =
        new MediaControlKeyManager(tabId, sInstanceId);
    manager->Open();
    mTabManagers[tabId] = manager;
  }
  LOG("Register media controller %" PRId64 ", currentNum=%" PRId64,
      aController->Id(), GetActiveControllersNum());
  if (StaticPrefs::media_mediacontrol_testingevents_enabled()) {
    if (nsCOMPtr<nsIObserverService> obs = services::GetObserverService()) {
      obs->NotifyObservers(nullptr, "media-controller-amount-changed", nullptr);
    }
  }
  return true;
}

bool MediaControlService::UnregisterActiveMediaController(
    MediaController* aController) {
  MOZ_DIAGNOSTIC_ASSERT(mControllerManager,
                        "Unregister controller before initializing service");
  if (!mControllerManager->RemoveController(aController)) {
    LOG("Fail to unregister controller %" PRId64, aController->Id());
    return false;
  }
  // Per-tab MPRIS: destroy the manager/service for this tab
  uint64_t tabId = aController->Id();
  auto it = mTabManagers.find(tabId);
  if (it != mTabManagers.end()) {
    it->second->Close();
    mTabManagers.erase(it);
  }
  LOG("Unregister media controller %" PRId64 ", currentNum=%" PRId64,
      aController->Id(), GetActiveControllersNum());
  if (StaticPrefs::media_mediacontrol_testingevents_enabled()) {
    if (nsCOMPtr<nsIObserverService> obs = services::GetObserverService()) {
      obs->NotifyObservers(nullptr, "media-controller-amount-changed", nullptr);
    }
  }
  return true;
}

void MediaControlService::NotifyControllerPlaybackStateChanged(
    MediaController* aController) {
  MOZ_DIAGNOSTIC_ASSERT(
      mControllerManager,
      "controller state change happens before initializing service");
  MOZ_DIAGNOSTIC_ASSERT(aController);
  // The controller is not an active controller.
  if (!mControllerManager->Contains(aController)) {
    return;
  }

  uint64_t tabId = aController->Id();
  auto it = mTabManagers.find(tabId);
  if (it != mTabManagers.end()) {
    it->second->SetPlaybackState(aController->PlaybackState());
    it->second->SetMediaMetadata(aController->GetCurrentMediaMetadata());
    it->second->SetSupportedMediaKeys(aController->GetSupportedMediaKeys());
    it->second->SetPositionState(aController->GetCurrentPositionState());
  }
  // No propagation of main controller state to other managers.
}

void MediaControlService::RequestUpdateMainController(
    MediaController* aController) {
  MOZ_DIAGNOSTIC_ASSERT(aController);
  MOZ_DIAGNOSTIC_ASSERT(
      mControllerManager,
      "using controller in PIP mode before initializing service");
  // The controller is not an active controller.
  if (!mControllerManager->Contains(aController)) {
    return;
  }
  mControllerManager->UpdateMainControllerIfNeeded(aController);
}

uint64_t MediaControlService::GetActiveControllersNum() const {
  MOZ_DIAGNOSTIC_ASSERT(mControllerManager);
  return mControllerManager->GetControllersNum();
}

MediaController* MediaControlService::GetMainController() const {
  MOZ_DIAGNOSTIC_ASSERT(mControllerManager);
  return mControllerManager->GetMainController();
}

void MediaControlService::GenerateTestMediaControlKey(MediaControlKey aKey,
                                                      double aSeekValue) {
  if (!StaticPrefs::media_mediacontrol_testingevents_enabled()) {
    return;
  }
  // Generate seek details when necessary
  switch (aKey) {
    case MediaControlKey::Seekto:
      mMediaKeysHandler->OnActionPerformed(MediaControlAction(
          aKey, SeekDetails(aSeekValue, false /* fast seek */)));
      break;
    case MediaControlKey::Seekbackward:
    case MediaControlKey::Seekforward:
      mMediaKeysHandler->OnActionPerformed(
          MediaControlAction(aKey, SeekDetails(aSeekValue)));
      break;
    default:
      mMediaKeysHandler->OnActionPerformed(MediaControlAction(aKey));
  }
}

MediaMetadataBase MediaControlService::GetMainControllerMediaMetadata() const {
  MediaMetadataBase metadata;
  if (!StaticPrefs::media_mediacontrol_testingevents_enabled()) {
    return metadata;
  }
  return GetMainController() ? GetMainController()->GetCurrentMediaMetadata()
                             : metadata;
}

MediaSessionPlaybackState MediaControlService::GetMainControllerPlaybackState()
    const {
  if (!StaticPrefs::media_mediacontrol_testingevents_enabled()) {
    return MediaSessionPlaybackState::None;
  }
  return GetMainController() ? GetMainController()->PlaybackState()
                             : MediaSessionPlaybackState::None;
}

nsString MediaControlService::GetFallbackTitle() const {
  return mFallbackTitle;
}

// Following functions belong to ControllerManager
MediaControlService::ControllerManager::ControllerManager(
    MediaControlService* aService) {}

bool MediaControlService::ControllerManager::AddController(
    MediaController* aController) {
  MOZ_DIAGNOSTIC_ASSERT(aController);
  if (mControllers.contains(aController)) {
    return false;
  }
  mControllers.insertBack(aController);
  mControllerMap[aController->Id()] = aController;
  UpdateMainControllerIfNeeded(aController);
  return true;
}

bool MediaControlService::ControllerManager::RemoveController(
    MediaController* aController) {
  MOZ_DIAGNOSTIC_ASSERT(aController);
  if (!mControllers.contains(aController)) {
    return false;
  }
  mControllerMap.erase(aController->Id());
  // This is LinkedListElement's method which will remove controller from
  // `mController`.
  static_cast<LinkedListControllerPtr>(aController)->remove();
  // If main controller is removed from the list, the last controller in the
  // list would become the main controller. Or reset the main controller when
  // the list is already empty.
  if (GetMainController() == aController) {
    UpdateMainControllerInternal(
        mControllers.isEmpty() ? nullptr : mControllers.getLast());
  }
  return true;
}

void MediaControlService::ControllerManager::UpdateMainControllerIfNeeded(
    MediaController* aController) {
  MOZ_DIAGNOSTIC_ASSERT(aController);

  if (GetMainController() == aController) {
    LOG_MAINCONTROLLER("This controller is alreay the main controller");
    return;
  }

  if (GetMainController() &&
      GetMainController()->IsBeingUsedInPIPModeOrFullscreen() &&
      !aController->IsBeingUsedInPIPModeOrFullscreen()) {
    LOG_MAINCONTROLLER(
        "Normal media controller can't replace the controller being used in "
        "PIP mode or fullscreen");
    return ReorderGivenController(aController,
                                  InsertOptions::eInsertAsNormalController);
  }
  ReorderGivenController(aController, InsertOptions::eInsertAsMainController);
  UpdateMainControllerInternal(aController);
}

void MediaControlService::ControllerManager::ReorderGivenController(
    MediaController* aController, InsertOptions aOption) {
  MOZ_DIAGNOSTIC_ASSERT(aController);
  MOZ_DIAGNOSTIC_ASSERT(mControllers.contains(aController));
  // Reset the controller's position and make it not in any list.
  static_cast<LinkedListControllerPtr>(aController)->remove();

  if (aOption == InsertOptions::eInsertAsMainController) {
    // Make the main controller as the last element in the list to maintain the
    // order of controllers because we always use the last controller in the
    // list as the next main controller when removing current main controller
    // from the list. Eg. If the list contains [A, B, C], and now the last
    // element C is the main controller. When B becomes main controller later,
    // the list would become [A, C, B]. And if A becomes main controller, list
    // would become [C, B, A]. Then, if we remove A from the list, the next main
    // controller would be B. But if we don't maintain the controller order when
    // main controller changes, we would pick C as the main controller because
    // the list is still [A, B, C].
    return mControllers.insertBack(aController);
  }

  MOZ_ASSERT(aOption == InsertOptions::eInsertAsNormalController);
  MOZ_ASSERT(GetMainController() != aController);
  // We might have multiple controllers which have higher priority (being used
  // in PIP or fullscreen) from the head, the normal controller should be
  // inserted before them. Therefore, search a higher priority controller from
  // the head and insert new controller before it.
  // Eg. a list [A, B, C, D, E] and D and E have higher priority, if we want
  // to insert F, then the final result would be [A, B, C, F, D, E]
  auto* current = static_cast<LinkedListControllerPtr>(mControllers.getFirst());
  while (!static_cast<MediaController*>(current)
              ->IsBeingUsedInPIPModeOrFullscreen()) {
    current = current->getNext();
  }
  MOZ_ASSERT(current, "Should have at least one higher priority controller!");
  current->setPrevious(aController);
}

void MediaControlService::ControllerManager::Shutdown() {
  mControllers.clear();
  mControllerMap.clear();
  DisconnectMainControllerEvents();
}

void MediaControlService::ControllerManager::UpdateMainControllerInternal(
    MediaController* aController) {
  mMainController = aController;
  if (nsCOMPtr<nsIObserverService> obs = services::GetObserverService()) {
    obs->NotifyObservers(nullptr, "main-media-controller-changed", nullptr);
  }
}
void MediaControlService::ControllerManager::DisconnectMainControllerEvents() {
  // No-op in single-instance logic
}

MediaController* MediaControlService::ControllerManager::GetMainController()
    const {
  return mMainController.get();
}

MediaController* MediaControlService::ControllerManager::GetControllerById(
    uint64_t aTabId) const {
  auto it = mControllerMap.find(aTabId);
  return it != mControllerMap.end() ? it->second : nullptr;
}

uint64_t MediaControlService::ControllerManager::GetControllersNum() const {
  return mControllers.length();
}

bool MediaControlService::ControllerManager::Contains(
    MediaController* aController) const {
  return mControllers.contains(aController);
}

}  // namespace mozilla::dom
