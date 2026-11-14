//
//  kern_coremedia.hpp — CoreMedia /Library codesign bypass for the media
//  processes.
//
//  Problem: CoreMedia's figRegistry scans /Library/Video/Plug-Ins with a
//  code-signature requirement (SecStaticCode vs
//  kFigRegistryAppleCodeSignRequirement). Our adhoc plugins fail it, so today
//  they must live in /System/Library/Video/Plug-Ins (scanned with no check),
//  which needs the sealed-volume root patch. This neutralises that check in
//  userspace inside com.apple.WebKit.GPU and VTDecoderXPCService only.
//
//  Scope note: the byte patch accepts every non-/System plug-in those two
//  processes discover under /Library (not VCN-only). AMFI still gates mapping
//  of adhoc code; AmfiBypass (or AMFIPass) must allow the library, and VCN
//  plugins are further constrained by their cdhash allow-list there.
//

#ifndef VCNVTKEXT_KERN_COREMEDIA_HPP_
#define VCNVTKEXT_KERN_COREMEDIA_HPP_

#include <Headers/kern_user.hpp>

namespace CoreMediaBypass {
// Register the UserPatcher proc + binary mods. Called from the plugin's
// onPatcherLoad.
void init();
}  // namespace CoreMediaBypass

#endif  // VCNVTKEXT_KERN_COREMEDIA_HPP_
