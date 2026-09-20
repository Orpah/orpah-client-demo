/* b64url.h — base64url（无填充），对应 orpah_id.b64url_encode
 *   Python: base64.urlsafe_b64encode(b).rstrip(b"=")
 *   ⇒ 字母表 A-Za-z0-9-_，**不带 '=' 填充**
 */
#ifndef ORPAH_B64URL_H
#define ORPAH_B64URL_H

#include <stddef.h>

/* 编码。out 需要 4*ceil(n/3) 字节（无 NUL）。返回写入字节数；缓冲不够返回 -1。 */
int b64url_encode(const unsigned char *in, size_t n, char *out, size_t cap);

#endif /* ORPAH_B64URL_H */
