---
title: Images and attachments
date: 2026-10-26
tags: ["goober", "images"]
template: post
image: HERO_IMAGE
---
Images live at `/attachment/$id.$ext`. An agent stores one with `attachment_put`, from base64
or an https URL, and a person can upload at [/attachment/](/attachment/). The type always
comes from the bytes, so an upload can never become a page.

![The same image, in a post](HERO_IMAGE)
