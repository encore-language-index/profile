# profile

Aggregated, named function profiling for Encore. Decorators are statically
expanded by the compiler, so the wrapped function is reached by a direct call.

```encore
import profile::ProfileManager

pub static RENDER_PROFILE = ProfileManager::new("render")

@RENDER_PROFILE.profile("draw_frame")
fn draw_frame() -> () {
    ret ()
}

fn main() -> u32 {
    draw_frame()
    RENDER_PROFILE.report()
    ret 0_u32
}
```

## SDK development

Encore pins this package as a Git submodule. Clone the complete SDK with
`git clone --recurse-submodules --branch trunk https://github.com/encore-ecosystem/encore.git`
and edit this package under `packages/profile/`. Sibling `path@` dependencies
are resolved from that checkout. Encore CI tests the exact package commits
with the compiler on all supported native targets. Published package archives
rewrite path dependencies to exact index versions.

Timing aggregates are collected per thread. Registered labels are stable numeric
identities: call `manager.register("label")` outside the hot loop and use
`record_registered(label, elapsed_ns)`. Legacy `record` uses a thread-local hash
lookup. The global registry is used only for manager creation; label registration
uses its manager's registry lock. Reporting copies individual aggregates under
short local locks and formats after releasing them. Reset advances a generation;
registered labels remain valid. Thread data is reclaimed at process shutdown.
`report()` writes to stdout; `report_text()` can be passed to any logger/sink.
The package no longer depends on log.
