#ifndef JPX_XAML_BACKEND_CMDBUF_H
#define JPX_XAML_BACKEND_CMDBUF_H

/* 把 jp_xaml 的 Direct 回放写到 VmCmdBuffer。
   这是 shell / app 使用 XAML 的直接后端（M7.1）；不依赖 Clay。 */

#include "jpx_xaml.h"
#include "vm_display.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 返回静态 backend，ctx 传 VmCmdBuffer*。
   文本度量走 vm_font（调用方需先 VmFont_LoadFile）。 */
const JpxXamlBackend *JpxXamlBackend_CmdBuf(void);

#ifdef __cplusplus
}
#endif

#endif
