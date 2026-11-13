# Make a language pack

Open the desktop **Modkit**, then **File → New language pack**. Choose the game
or an installed mod, enter the target language code (`fr`, `pt-BR`, `es-MX`,
for example), your translator credit and a pack name. Game text is read from
your own installed files with the bundled `modkit_streams` tool. A mod needs a
`default_locale` and a source catalog; all of the small starter projects include
both.

The translation workspace lists the catalogs and shows original text beside
your translation. Search the current table, filter untranslated entries and
watch the translated/remaining counts. **Keep the original text** leaves that
entry out of your pack. Uncheck it to write a translation, including multiple
lines. **Insert tag** copies a button tag or placeholder exactly.

Use **Save table** to keep a draft. Source text and text IDs stay intact;
another editor's changed revision cannot be overwritten silently. Drafts may
contain unfinished tags. **Check pack** reports missing or changed placeholders
and font limitations, and export requires those errors to be resolved.

**Export language pack** builds and exports the installable ZIP in one step.
It includes your translations and credit, with the required mod dependency
when translating another mod. It does not include the original game archives
or your `.translations` working catalogs. Partial packs are supported: missing
entries fall back to the original text.

Keep the entire project, including `.translations`, to continue later. Select
the project and choose **File → Edit translations** to reopen it. Share the ZIP
with players; they import it in **Mods** on desktop or Android and select the
matching language. A game pack replaces the installed source language chosen
when creating it, so use that source language in the game's own settings.

The current font checks protect characters that the installed font cannot
display. A language needing additional glyphs or right-to-left layout also
needs a supported font/layout import; the editor will not silently replace
those characters with question marks.

The existing `modkit translate new/edit/check/build` commands use the same
catalogs and validation. Advanced users can keep using those commands or the
browser editor without converting projects.
