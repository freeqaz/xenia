/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * RB3 / RB3DX (title 45410914) title module (NOT upstream).
 *
 * RegisterRb3TitleHooks() is called by xenia-headless only.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_RB3_RB3_TITLE_H_
#define XENIA_TITLES_RB3_RB3_TITLE_H_

namespace xe {
namespace titles {
struct TitleLaunchContext;
}  // namespace titles

// The RB3 blocks of Emulator::CompleteLaunch, moved verbatim.
void ApplyRb3LaunchHooks(const titles::TitleLaunchContext& ctx);

void RegisterRb3TitleHooks();

}  // namespace xe

#endif  // XENIA_TITLES_RB3_RB3_TITLE_H_
