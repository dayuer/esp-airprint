/**
 * RFC 5987 的 ext-value 编码，给 `X-Filename` 用（docs/API-cloud-print.md 4.4）。
 * 仅用于显示，但编错了用户在作业列表里看到的就是一串乱码。
 *
 * 用 encodeURIComponent 而不是 TextEncoder：前者本来就做 UTF-8 百分号编码，
 * 且在 Hermes 上一定有；TextEncoder 在 RN 的类型库里都没有。
 *
 * 两者的字符集差一点：encodeURIComponent 会放过 `*'()`，而 RFC 5987 的
 * attr-char 不允许它们，所以补一道。反过来它多编了 `#$&+^|` 和反引号——
 * 多编是合法的，不用管。
 */
export function encodeRfc5987(filename: string): string {
  const encoded = encodeURIComponent(filename).replace(
    /[*'()]/g,
    c => '%' + c.charCodeAt(0).toString(16).toUpperCase(),
  );
  return `UTF-8''${encoded}`;
}
