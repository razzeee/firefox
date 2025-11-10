/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

#ifndef WIDGET_MEDIAKEYSEVENTSOURCEFACTORY_H_
#define WIDGET_MEDIAKEYSEVENTSOURCEFACTORY_H_

// Needed for uint32_t
#include <cstdint>
namespace mozilla {
namespace dom {
class MediaControlKeySource;
}  // namespace dom
}  // namespace mozilla

namespace mozilla {
namespace widget {

// This function declaration is used to create a media keys event source on
// different platforms, each platform should have their own implementation.
#if defined(MOZ_WIDGET_GTK)
extern mozilla::dom::MediaControlKeySource* CreateMediaControlKeySource(
    uint32_t instanceId, uint32_t tabId);
#else
extern mozilla::dom::MediaControlKeySource* CreateMediaControlKeySource();
#endif

}  // namespace widget
}  // namespace mozilla

#endif
