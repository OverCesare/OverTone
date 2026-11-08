"use strict";
const config = window.OVERTONE_SITE || {};
const clips = window.OVERTONE_CLIPS || [];
const player = document.getElementById("clip-video");
const playButton = document.getElementById("clip-play");
const tabs = [...document.querySelectorAll("[data-clip]")];
function safeURL(value) {
  try { const u = new URL(value); return u.protocol === "https:" ? u.href : null; }
  catch { return null; }
}
const repo = safeURL(config.repository);
if (repo) document.querySelectorAll(".repo-link").forEach(link => { link.href = repo; link.hidden = false; });
const canonical = safeURL(config.canonical);
if (canonical) {
  let link = document.querySelector('link[rel="canonical"]'); if (!link) { link = document.createElement("link"); link.rel = "canonical"; document.head.append(link); } link.href = canonical;
}
async function play(video) {
  try { await video.play(); }
  catch { /* Native controls remain available if playback is denied. */ }
}
function selectClip(id, focusTab = false) {
  const clip = clips.find(item => item.id === id);
  if (!clip || !player) return;
  player.pause();
  player.src = clip.video;
  player.poster = clip.poster;
  player.setAttribute("aria-label", clip.title);
  player.load();
  playButton.hidden = false;
  playButton.setAttribute("aria-label", "Play " + clip.title);
  document.getElementById("clip-title").textContent = clip.title;
  document.getElementById("clip-description").textContent = clip.description;
  document.getElementById("clip-number").textContent = clip.id;
  document.getElementById("clip-duration").textContent = clip.duration + " s";
  document.getElementById("clip-file").href = clip.video;
  document.getElementById("clip-panel").setAttribute("aria-labelledby", "tab-" + clip.id);
  document.getElementById("clip-select").value = clip.id;
  tabs.forEach(tab => {
    const selected = tab.dataset.clip === id;
    tab.setAttribute("aria-selected", String(selected)); tab.tabIndex = selected ? 0 : -1;
    if (selected && focusTab) tab.focus();
  });
}
if (player) {
  playButton.addEventListener("click", () => play(player));
  player.addEventListener("play", () => { playButton.hidden = true; });
  player.addEventListener("ended", () => { playButton.hidden = false; });
  tabs.forEach((tab, index) => {
    tab.addEventListener("click", () => selectClip(tab.dataset.clip));
    tab.addEventListener("keydown", event => {
      let next = null;
      if (event.key === "ArrowRight") next = (index + 1) % tabs.length;
      if (event.key === "ArrowLeft") next = (index + tabs.length - 1) % tabs.length;
      if (event.key === "Home") next = 0;
      if (event.key === "End") next = tabs.length - 1;
      if (next !== null) { event.preventDefault(); selectClip(tabs[next].dataset.clip, true); }
    });
  });
  document.getElementById("clip-select").addEventListener("change", event => selectClip(event.target.value));
}
const trailerCover = document.getElementById("trailer-cover");
if (trailerCover) {
  const videoId = trailerCover.dataset.videoId;
  if (/^[a-zA-Z0-9_-]{11}$/.test(videoId || "")) {
    trailerCover.addEventListener("click", () => {
      if (player) player.pause();
      const frame = document.createElement("iframe");
      frame.title = "OverTone 0.1 — YouTube trailer";
      frame.width = "1920";
      frame.height = "1080";
      frame.allow = "autoplay; encrypted-media; picture-in-picture; web-share";
      frame.allowFullscreen = true;
      frame.referrerPolicy = "strict-origin-when-cross-origin";
      frame.src = "https://www.youtube-nocookie.com/embed/" + videoId + "?autoplay=1&playsinline=1&rel=0";
      trailerCover.replaceWith(frame);
      frame.focus({preventScroll:true});
    }, {once:true});
    trailerCover.disabled = false;
  }
}
document.addEventListener("visibilitychange", () => { if (document.hidden && player) player.pause(); });
