{
  "targets": [
    {
      "target_name": "screen_capture_addon",
      "sources": [
        "src/addon.cpp",
        "src/serialize.cpp"
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "cflags_cc": [
        "-std=c++20",
        "-fexceptions"
      ],
      "conditions": [
        ["OS=='win'", {
          "sources": [
            "src/win/platform_capture_win.cpp"
          ],
          "msvs_settings": {
            "VCCLCompilerTool": {
              "AdditionalOptions": [
                "/EHsc",
                "/std:c++20"
              ]
            }
          },
          "libraries": [
            "runtimeobject.lib",
            "d3d11.lib",
            "dxgi.lib"
          ]
        }],
        ["OS=='linux'", {
          "sources": [
            "src/linux/platform_capture_linux.cpp"
          ],
          "cflags_cc": [
            "<!@(pkg-config --cflags gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 libpipewire-0.3)"
          ],
          "libraries": [
            "<!@(pkg-config --libs gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 libpipewire-0.3)"
          ]
        }],
        ["OS!='win' and OS!='linux'", {
          "sources": [
            "src/platform_capture_stub.cpp"
          ]
        }]
      ]
    }
  ]
}