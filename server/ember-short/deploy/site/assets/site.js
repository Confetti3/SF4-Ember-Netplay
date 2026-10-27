// What the rooms, rankings and open pages share: the release data that is
// edited at each release, and the two small helpers they used to carry each.
// A page loads this before its own script (both deferred, so in order) and
// reads window.EmberSite.
(function () {
  'use strict';

  // The Stable release a tournament match asks for. Both fighters need the
  // same build, so a match page names this one; bump it with each release,
  // as VERSIONS below names each room build.
  var STABLE_VERSION = '1.4.1';

  // Sidecar build IDs of the releases public rooms run, newest first. Add the
  // new one at each release; a room on a build not listed here says
  // "Other version".
  var VERSIONS = [
    ['fa57472adffd80ed65c943577b4cba204a5673640a167d5c94b83f89bb4fbd77', '1.3.1'],
    ['ed1933800aa0099c4ebd435db3e2d6d174666121079301913df3ffa239de3c8d', '1.4.0'],
    ['5161a3978f7cba2aabf1270f6a88143a6e5f87d8bd808010a60108a4fa7a1469', '1.4.1'],
    ['1668cadbd970fd75b019ff6c1f4f56eb6f0712c16013b3e5c50f29aac32c0b38', '1.3.0'],
    ['77b8c778dc1a4dc81ef0de4dbff808057534e920706eb65977a1d39f6eb2bd63', '1.3.0 Nightly'],
    ['900d86d6dc991a71378dd626bbe0ad1c77d1006215745cca406f96e684d4546d', '1.3.0 Nightly'],
    ['484be955acd1cbd11350b4672191306048fd32984f8c71ffc1b3953808da4884', '1.2.0'],
    ['1b21c6a92db3e9009edfa0fe9643b67db05fc430c86819363908284b5e7502da', '1.2.0 Nightly'],
    ['400f36a4a4353c86e8f2c294b4490f287895f9849137daa284ce822e1596b2cd', '1.1.2'],
    ['3581000c7b42636e90b6c14dc378cad67c621f8bc82ae6f0900476323c5723d0', '1.1.1'],
    ['d3dafb0b0a96f8a0152ac6edf76d79272ed085e618cc4dcc656018a918f56166', '1.1.0'],
    ['1ba5ca463447b379cdd39d266942dc0f7171215dbf502454e281da4ccd86f8ea', '1.1.0-rc2'],
    ['cc4a0d6ce968fbaa99fe7653b02b80a083d18f1423c27f4f5376a07f01adc863', '1.1.0-rc1']
  ];

  function el(tag, className, text) {
    var node = document.createElement(tag);
    if (className) node.className = className;
    if (text !== undefined) node.textContent = text;
    return node;
  }

  // Copies text and says so in the status element: done on success, failed
  // when neither the clipboard nor the old copy command works. The fallback
  // textarea is hidden by the offscreen class, or by inline style when the
  // page has no such class use (inline true).
  function copy(status, text, done, failed, inline) {
    function fallback() {
      var area = document.createElement('textarea');
      area.value = text;
      area.setAttribute('readonly', '');
      if (inline) {
        area.style.position = 'fixed';
        area.style.opacity = '0';
      } else {
        area.className = 'offscreen';
      }
      document.body.appendChild(area);
      area.select();
      var ok = false;
      try { ok = document.execCommand('copy'); } catch (error) { ok = false; }
      document.body.removeChild(area);
      status.textContent = ok ? done : failed;
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { status.textContent = done; }, fallback);
    } else {
      fallback();
    }
  }

  window.EmberSite = { STABLE_VERSION: STABLE_VERSION, VERSIONS: VERSIONS, el: el, copy: copy };
}());
