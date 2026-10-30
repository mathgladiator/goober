// The built-in templates: a base layout, a page, and a list, so a tenant with no
// templates still gets clean, responsive pages (007 §5). A tenant or the _host tree
// overrides any of them by writing _templates/$name.html.

#include <map>
#include <string>

namespace goober::site {

const std::map<std::string, std::string>& builtin_templates() {
  static const std::map<std::string, std::string> kTemplates = {
      {"base", R"html(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{{title}}{{#site.title}} · {{site.title}}{{/site.title}}</title>
{{#summary}}<meta name="description" content="{{summary}}">{{/summary}}
{{#canonical}}<link rel="canonical" href="{{canonical}}">{{/canonical}}
{{#pagination.has_prev}}<link rel="prev" href="{{pagination.prev_url}}">{{/pagination.has_prev}}
{{#pagination.has_next}}<link rel="next" href="{{pagination.next_url}}">{{/pagination.has_next}}
<meta property="og:title" content="{{title}}">
<meta property="og:type" content="{{og_type}}">
{{#canonical}}<meta property="og:url" content="{{canonical}}">{{/canonical}}
{{#summary}}<meta property="og:description" content="{{summary}}">{{/summary}}
{{#site.title}}<meta property="og:site_name" content="{{site.title}}">{{/site.title}}
{{#image_url}}<meta property="og:image" content="{{image_url}}">{{/image_url}}
<meta name="twitter:card" content="{{#image_url}}summary_large_image{{/image_url}}{{^image_url}}summary{{/image_url}}">
{{#author}}<meta name="author" content="{{author}}">{{/author}}
{{#noindex}}<meta name="robots" content="noindex">{{/noindex}}
{{{json_ld}}}
{{{head}}}
<!--#include pwa-->
<style>
:root{color-scheme:light dark;--fg:#1d1d1f;--bg:#fbfbf8;--muted:#6b6b6b;--line:#e3e1da;--accent:#2457d6;--card:#fff}
@media (prefers-color-scheme:dark){:root{--fg:#ececec;--bg:#141414;--muted:#9a9a9a;--line:#2c2c2c;--accent:#7aa2ff;--card:#1c1c1c}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--fg);font:17px/1.6 system-ui,-apple-system,"Segoe UI",sans-serif}
a{color:var(--accent)}
header.site,main,footer.site{max-width:44rem;margin:0 auto;padding:0 16px}
header.site{display:flex;flex-wrap:wrap;gap:.5rem 1.25rem;align-items:baseline;padding-top:1.5rem;padding-bottom:1rem;border-bottom:1px solid var(--line)}
header.site .brand{font-weight:700;color:var(--fg);text-decoration:none;font-size:1.1rem}
header.site nav{display:flex;flex-wrap:wrap;gap:1rem}
header.site nav a{color:var(--muted);text-decoration:none}
header.site nav a:hover{color:var(--accent)}
main{padding-top:1.5rem;padding-bottom:3rem}
h1,h2,h3{line-height:1.25}
img{max-width:100%;height:auto;border-radius:6px}
pre{overflow-x:auto;background:var(--card);border:1px solid var(--line);border-radius:8px;padding:.75rem 1rem}
code{font-size:.92em}
blockquote{margin:1rem 0;padding:.1rem 1rem;border-left:3px solid var(--line);color:var(--muted)}
table{border-collapse:collapse;width:100%;display:block;overflow-x:auto}
th,td{border-bottom:1px solid var(--line);padding:.4rem .6rem;text-align:left}
.meta{color:var(--muted);font-size:.9rem}
ul.entries{list-style:none;padding:0}
ul.entries li{padding:1rem 0;border-bottom:1px solid var(--line)}
ul.entries h2{margin:.1rem 0;font-size:1.2rem}
ul.entries img{max-height:12rem;object-fit:cover;width:100%;margin-bottom:.5rem}
ul.entries p{margin:.3rem 0 0}
nav.pages{display:flex;gap:1rem;justify-content:space-between;margin-top:1.5rem}
form label{display:block;margin:.5rem 0;font-weight:600}
input,textarea,select{display:block;width:100%;margin-top:.25rem;font:inherit;font-weight:400;padding:.5rem .6rem;border:1px solid var(--line);border-radius:6px;background:var(--card);color:var(--fg)}
button{font:inherit;font-weight:600;padding:.5rem 1rem;border:0;border-radius:6px;background:var(--accent);color:#fff;cursor:pointer}
footer.site{padding-top:1rem;padding-bottom:2rem;border-top:1px solid var(--line);color:var(--muted);font-size:.9rem}
</style>
{{#site.stylesheet}}<link rel="stylesheet" href="{{site.stylesheet}}">{{/site.stylesheet}}
{{#page.feed}}<link rel="alternate" type="application/atom+xml" title="{{title}}" href="{{page.feed_url}}">
<link rel="alternate" type="application/rss+xml" title="{{title}}" href="{{page.rss_url}}">{{/page.feed}}
</head>
<body>
<header class="site">
<a class="brand" href="/">{{#site.title}}{{site.title}}{{/site.title}}{{^site.title}}Home{{/site.title}}</a>
<nav>{{#site.nav}}<a href="{{url}}">{{label}}</a>{{/site.nav}}</nav>
</header>
<main>
{{{body}}}
</main>
<footer class="site">{{#site.footer}}{{site.footer}}{{/site.footer}}{{^site.footer}}© {{now.year}} {{site.title}}{{/site.footer}}</footer>
</body>
</html>
)html"},
      {"page", R"html(---
layout: base
---
<article class="page">
{{^title_in_body}}<h1>{{title}}</h1>{{/title_in_body}}
{{#date}}<p class="meta"><time datetime="{{date}}">{{date}}</time></p>{{/date}}
{{{body}}}
</article>
)html"},
      {"post", R"html(---
layout: base
---
<article class="post">
<h1>{{title}}</h1>
<p class="meta">{{#author}}By {{author}}{{/author}}{{#author}}{{#date}} · {{/date}}{{/author}}{{#date}}<time datetime="{{date}}">{{date}}</time>{{/date}}</p>
{{#image}}<img src="{{image}}" alt="" class="hero">{{/image}}
{{{body}}}
{{#has_tags}}<p class="meta">Tagged {{#tags}}<span class="tag">{{.}}</span> {{/tags}}</p>{{/has_tags}}
</article>
)html"},
      {"list", R"html(---
layout: base
---
<article class="list">
{{^title_in_body}}<h1>{{title}}</h1>{{/title_in_body}}
{{{body}}}
{{#tag}}<p class="meta">Tagged <strong>{{tag}}</strong> · <a href="{{page.url}}">show all</a></p>{{/tag}}
<ul class="entries">
{{#pages}}
<li>
{{#image}}<a href="{{url}}"><img src="{{image}}" alt="" loading="lazy"></a>{{/image}}
<h2><a href="{{url}}">{{title}}</a></h2>
<p class="meta">{{#author}}{{author}} · {{/author}}{{#date}}<time datetime="{{date}}">{{date}}</time>{{/date}}</p>
{{#summary}}<p>{{summary}}</p>{{/summary}}
</li>
{{/pages}}
</ul>
{{^pages}}<p class="meta">Nothing here yet.</p>{{/pages}}
{{#pagination}}
<nav class="pages">
<span>{{#has_prev}}<a rel="prev" href="{{prev_url}}">← Newer</a>{{/has_prev}}</span>
<span class="meta">Page {{page}} of {{pages}}</span>
<span>{{#has_next}}<a rel="next" href="{{next_url}}">Older →</a>{{/has_next}}</span>
</nav>
{{/pagination}}
</article>
)html"},
  };
  return kTemplates;
}

}  // namespace goober::site
