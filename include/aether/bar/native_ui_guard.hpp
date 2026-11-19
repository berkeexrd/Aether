#pragma once
// Windows' own shell surfaces (Start, Search, Widgets, the quick settings / notification / calendar flyouts) are replaced by Aether's own.
// While the bar runs they are not allowed to stay alive: the host processes are ended as soon as Windows starts them, so none of them
// can pop up over the bar. Windows simply starts them again after Aether has been closed.
namespace aether::bar {

class NativeUiGuard {
public:
    static void Start();
    static void Stop();
};

} // namespace aether::bar
