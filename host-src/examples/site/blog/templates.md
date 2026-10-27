---
title: Templates with layouts
date: 2026-10-24
tags: ["goober", "templates"]
template: post
---
This post uses the `post` template, which starts with `layout: base`, so its output becomes
the `body` of the base layout. Values are always escaped; only the page's rendered `body` is
placed as HTML with `{{{body}}}`.
