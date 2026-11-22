#ifndef VM_CLIPBOARD_H
#define VM_CLIPBOARD_H

#ifdef __cplusplus
extern "C" {
#endif

/* guest 内单例剪贴板：所有窗口共享一份 UTF-8 文本。
   跨端协议不传剪贴板，复制粘贴完全在 guest 内完成。 */

#define VM_CLIPBOARD_CAP 4096

void VmClipboard_SetText(const char *utf8);
/* 写入最多 cap-1 字节并补 '\0'，返回实际写入字节数；无内容返回 0。 */
int VmClipboard_GetText(char *out, int cap);
int VmClipboard_HasText(void);
void VmClipboard_Clear(void);

#ifdef __cplusplus
}
#endif

#endif
