This repository includes a patch for the final version of
[pinpoint](https://wiki.gnome.org/action/show/Attic/Pinpoint) (discontinued),
which allows you to generate PDFs of your presentation without running X.

The original (historic) repo can be found at
https://gitlab.gnome.org/Archive/pinpoint

Original documentation below.

---

pinpoint
========

Pinpoint a simple presentation tool that hopes to avoid audience death by
bullet point and instead encourage presentations containing beautiful images
and small amounts of concise text in slides.

Pinpoint’s main web presence is a page on the GNOME wiki:

 http://wiki.gnome.org/Apps/Pinpoint

A mailinglist for pinpoint development and use is set up, it's info page is at:

 http://mail.gnome.org/mailman/listinfo/pinpoint-list

Some of pinpoints core features are:

 * Text position
 * Styling of font, text-color, contrast background and text positioning for
   global default and per slide overrides.
 * Image backgrounds
 * Video backgrounds
 * Pango markup inside slides
 * Transitions, extendable through json
 * PDF export
 * Embedding commands to run for demos in slides, with editable commandline
   during presentation.
 * Monitoring of source file with live updates of changed slide for authoring


The following illustrates what a pinpoint presentation looks like, for a more
exhaustive overview of pinpoints features see the included sample presentation.

example pinpoint presentation
-----------------------------

```
# the 0th "slide" provides default styling for the presentation
[bottom]           # position of text
[slide-bg.jpg]     # default slide background
--- [black] [center] # override background and text position

A presentation

--------- # lines starting with hyphens separate slides

The format is meant to be <u>simple</u>

--- [ammo.jpg]  # override background

• Bullet point lists through unicode
• Evil, but sometimes needed
```

For more details see introduction.pin in the pinpoint install.
