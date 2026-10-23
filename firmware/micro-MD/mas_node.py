# lib/mas のうち基板側のコア（純粋な C）だけをビルドに足す。
# library.json は Arduino 向けで、主制御の C++ を含むため lib_deps では取り込まない。
Import("env")

env.Prepend(
    LIBS=[
        env.BuildLibrary(
            "$BUILD_DIR/mas",
            "$PROJECT_DIR/../../lib/mas/src/can",
            src_filter=["-<*>", "+<node.c>", "+<core/md_frame.c>", "+<core/setting.c>"],
        )
    ]
)
