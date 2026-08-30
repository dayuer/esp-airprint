/**
 * @jest-environment node
 */
import {encodeRfc5987} from '../rfc5987';

// 黄金用例取自文档 4.4 的 X-Filename 示例。
test('中文文件名按文档示例编码', () => {
  expect(encodeRfc5987('报告.pdf')).toBe("UTF-8''%E6%8A%A5%E5%91%8A.pdf");
});

test('ASCII 文件名里的安全字符不编码', () => {
  expect(encodeRfc5987('report-2026_v1.pdf')).toBe("UTF-8''report-2026_v1.pdf");
});

test('空格和引号必须编码', () => {
  expect(encodeRfc5987('my report".pdf')).toBe("UTF-8''my%20report%22.pdf");
});

test('emoji 走多字节 UTF-8', () => {
  expect(encodeRfc5987('🖨.pdf')).toBe("UTF-8''%F0%9F%96%A8.pdf");
});
