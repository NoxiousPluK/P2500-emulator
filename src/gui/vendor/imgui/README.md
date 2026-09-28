# Dear ImGui (vendored)

Dear ImGui v1.92.1, MIT licensed — see `LICENSE.txt`. Vendored rather than
packaged because it is not in the Arch repositories and because ImGui is
designed to be built as part of the application, the same treatment
`src/core/vendor/superzazu_z80` gets.

Only what this project uses is kept: the four core `.cpp` files, their
headers, and the `imgui_impl_sdl3` + `imgui_impl_sdlrenderer3` backends so
the debugger UI shares the front-end's existing `SDL_Renderer`. The demo,
the other twenty-odd backends, the examples and the docs are not vendored.

**Nothing outside `src/gui/` may include this.** `libp2500.a` is C11 with no
dependencies and must stay that way.

Upgrading: replace these files from a release tarball and rebuild. There are
no local modifications.
