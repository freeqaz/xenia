/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * DC3 (title 373307D9) title module (NOT upstream).
 *
 * RegisterDc3TitleHooks() is called by xenia-headless only.
 ******************************************************************************
 */

#ifndef XENIA_TITLES_DC3_DC3_TITLE_H_
#define XENIA_TITLES_DC3_DC3_TITLE_H_

namespace xe {
namespace titles {
struct TitleLaunchContext;
}  // namespace titles

// The DC3 block of Emulator::CompleteLaunch, moved verbatim.
void ApplyDc3LaunchHooks(const titles::TitleLaunchContext& ctx);

void RegisterDc3TitleHooks();

}  // namespace xe

#endif  // XENIA_TITLES_DC3_DC3_TITLE_H_
