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
