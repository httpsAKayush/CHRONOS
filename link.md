This happened due to a combination of two things in Linux/C++ development: Symlinks and RPATHs.

Here is exactly how it works under the hood:

1. The Broken Shortcut (Symlink)
When you ran the First-Time Setup earlier, you ran: ln -s $(pwd)/chronos ~/.local/bin/chronos

This created a "symlink" (a shortcut). Unlike Windows shortcuts which try to track file movements, Linux symlinks are completely "dumb"—they just store a hardcoded string of the target path. Your shortcut literally just contained the text: /home/zer0/CHRONO/build/chronos. When you renamed the folder to CHRONOS, the shortcut was still pointing to CHRONO. The shell looked at it, saw the target didn't exist, and ignored it.

2. The Global Fallback
Because your terminal couldn't use the broken ~/.local/bin/chronos shortcut, it kept searching your system's $PATH for any other program named chronos. It ended up finding an extremely old version of chronos sitting in /usr/local/bin/chronos (from a time when you previously ran sudo make install). The terminal ran that old executable instead of your new one.

3. The "Hardcoded" Library Paths (RPATH)
So why did that old executable throw the libtree-sitter-cpp.so error?

When CMake compiles a C++ program, it "bakes" the absolute paths of shared libraries (.so files) directly into the executable's binary header. This is called an RPATH (Run-Time Search Path).

In the build folder, CMake hardcodes the absolute paths (e.g., /home/zer0/CHRONO/...) so you can run the program immediately without installing it.
When you run sudo make install, CMake typically strips those absolute build paths out of the binary for security reasons.
Because the shell ran the old, globally installed binary (which had its build paths stripped), it had no idea where to find libtree-sitter-cpp.so (which is sitting locally in your ~/.local/lib folder, not in the global /usr/lib).

Summary: Renaming the folder broke the shortcut > the terminal fell back to an old, globally installed binary > that old binary had its library paths stripped by CMake > it couldn't find the tree-sitter libraries. Rebuilding and fixing the symlink resolved the entire chain!