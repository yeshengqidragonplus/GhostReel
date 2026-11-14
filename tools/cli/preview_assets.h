// The files of the preview page, embedded in the executable. The table is generated at build time by
// embed_assets.cmake from the list in CMakeLists.txt (tools/preview/www and the master shader in shaders/).
#pragma once

#include <cstddef>

namespace cli {

struct PreviewAsset {
  const char* url;           // "/app.js"
  const char* source;        // path below the repository root: "tools/preview/www/app.js" (used by --dev-root)
  const char* content_type;  // "application/javascript"
  const unsigned char* data;
  size_t size;
};

extern const PreviewAsset kPreviewAssets[];
extern const size_t kPreviewAssetCount;

}  // namespace cli
