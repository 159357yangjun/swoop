const assert = require("assert");
const fs = require("fs");
const path = require("path");

const contentPath = path.join(__dirname, "content.js");
assert.ok(fs.existsSync(contentPath), "content.js must exist");
const { collectCandidates } = require(contentPath);
assert.strictEqual(typeof collectCandidates, "function");

function element(attributes) {
  return {
    getAttribute(name) {
      return attributes[name] || null;
    },
    href: attributes.href || "",
    src: attributes.src || "",
    tagName: attributes.tagName || "A",
  };
}

const nodes = [
  element({ href: "https://example.com/video.mp4", tagName: "A" }),
  element({ href: "https://example.com/video.mp4", tagName: "A" }),
  element({ src: "https://cdn.example.com/lesson.webm", tagName: "VIDEO" }),
  element({ src: "data:video/mp4;base64,ignore", tagName: "VIDEO" }),
  element({ href: "mailto:ignore@example.com", tagName: "A" }),
];
const fakeDocument = {
  title: "Lesson",
  querySelectorAll(selector) {
    if (selector === "a[href]") return nodes.filter((n) => n.tagName === "A");
    return nodes.filter((n) => n.tagName !== "A");
  },
};

const result = collectCandidates(fakeDocument, "https://example.com/course");
assert.strictEqual(result.length, 2);
assert.deepStrictEqual(result.map((item) => item.url), [
  "https://example.com/video.mp4",
  "https://cdn.example.com/lesson.webm",
]);
assert.strictEqual(result[0].type, "video");
assert.strictEqual(result[0].filename, "video.mp4");
console.log("content candidate tests: PASS");
