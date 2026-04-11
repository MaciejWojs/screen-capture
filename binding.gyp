{
  "targets": [
    {
      "target_name": "screen_capture_addon",
      "variables": {
        "force_api%": "<!(node -p \"process.env.force_api || 'auto'\")"
      },
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
        "NAPI_DISABLE_CPP_EXCEPTIONS",
        "GYP_FORCE_API=\"<(force_api)\""
      ],
      "cflags_cc": [
        "-std=c++20",
        "-fexceptions"
      ],

    "conditions": [
        ["force_api=='winrt'", {
          "defines": ["FORCE_API_WINRT"]
        }],
        ["force_api=='dxgi'", {
          "defines": ["FORCE_API_DXGI"]
        }],
        ["force_api=='gdi'", {
          "defines": ["FORCE_API_GDI"]
        }],

        ["OS=='win'", {
          "sources": [
            "src/win/capture_winrt.cpp",
            "src/win/capture_dxgi.cpp",
            "src/win/capture_gdi.cpp",
            "src/win/capture_factory.cpp"
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
            "<!@(pkg-config --cflags gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 libpipewire-0.3 x11 xext)"
          ],
          "libraries": [
            "<!@(pkg-config --libs gio-2.0 gio-unix-2.0 glib-2.0 gobject-2.0 libpipewire-0.3 x11 xext)"
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