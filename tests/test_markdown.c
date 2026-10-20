#include "test.h"
#include "html.h"
#include "markdown.h"
#include "extract.h"

static char *md_of(const char *html, bool drop_boiler) {
    rbl_node *root = rbl_html_parse(html, strlen(html));
    rbl_node *content = drop_boiler ? rbl_find_main_content(root) : root;
    char *md = rbl_render_markdown(content, "https://ex.de/base/", drop_boiler);
    rbl_node_free(root);
    return md;
}

void test_markdown(void) {
    char *md;

    md = md_of("<h1>Titel</h1><p>Hallo <b>Welt</b> und <i>mehr</i>.</p>", false);
    CHECK_CONTAINS(md, "# Titel");
    CHECK_CONTAINS(md, "**Welt**");
    CHECK_CONTAINS(md, "*mehr*");
    free(md);

    md = md_of("<h2>A</h2><h3>B</h3>", false);
    CHECK_CONTAINS(md, "## A");
    CHECK_CONTAINS(md, "### B");
    free(md);

    /* links resolve against base */
    md = md_of("<a href=\"../x.html\">klick</a>", false);
    CHECK_CONTAINS(md, "[klick](https://ex.de/x.html)");
    free(md);

    /* images */
    md = md_of("<img src=\"/img/logo.png\" alt=\"Logo\">", false);
    CHECK_CONTAINS(md, "![Logo](https://ex.de/img/logo.png)");
    free(md);

    /* unordered list */
    md = md_of("<ul><li>eins</li><li>zwei</li></ul>", false);
    CHECK_CONTAINS(md, "- eins");
    CHECK_CONTAINS(md, "- zwei");
    free(md);

    /* ordered list */
    md = md_of("<ol><li>a</li><li>b</li></ol>", false);
    CHECK_CONTAINS(md, "1. a");
    CHECK_CONTAINS(md, "2. b");
    free(md);

    /* nested list indentation */
    md = md_of("<ul><li>top<ul><li>child</li></ul></li></ul>", false);
    CHECK_CONTAINS(md, "- top");
    CHECK_CONTAINS(md, "  - child");
    free(md);

    /* blockquote */
    md = md_of("<blockquote><p>zitat</p></blockquote>", false);
    CHECK_CONTAINS(md, "> zitat");
    free(md);

    /* code block */
    md = md_of("<pre><code>int x = 1;</code></pre>", false);
    CHECK_CONTAINS(md, "```");
    CHECK_CONTAINS(md, "int x = 1;");
    free(md);

    /* inline code */
    md = md_of("<p>nutze <code>printf()</code> hier</p>", false);
    CHECK_CONTAINS(md, "`printf()`");
    free(md);

    /* table */
    md = md_of("<table><tr><th>Name</th><th>Wert</th></tr>"
               "<tr><td>a</td><td>1</td></tr></table>",
               false);
    CHECK_CONTAINS(md, "| Name | Wert |");
    CHECK_CONTAINS(md, "| --- |");
    CHECK_CONTAINS(md, "| a | 1 |");
    free(md);

    /* boilerplate dropped: nav/footer should disappear, article kept */
    md = md_of("<body><nav><a href=\"/\">Home</a></nav>"
               "<article><h1>Inhalt</h1><p>Das ist der Haupttext mit genug "
               "Woertern, damit die Heuristik den Artikel als Hauptinhalt "
               "auswaehlt und die Navigation verwirft.</p></article>"
               "<footer>Impressum Footerzeug</footer></body>",
               true);
    CHECK_CONTAINS(md, "# Inhalt");
    CHECK_CONTAINS(md, "Haupttext");
    CHECK(strstr(md, "Impressum") == NULL);
    free(md);

    /* entities already decoded in text */
    md = md_of("<p>Gr&uuml;&szlig;e</p>", false);
    CHECK_CONTAINS(md, "Grüße");
    free(md);

    /* markdown special chars in text are escaped */
    md = md_of("<p>a*b_c[d]</p>", false);
    CHECK_CONTAINS(md, "a\\*b");
    free(md);

    /* regression: nested inline code keeps document order */
    md = md_of("<p><code>a<b>B</b>c</code></p>", false);
    CHECK_CONTAINS(md, "`aBc`");
    free(md);

    /* regression: nested <pre> keeps document order */
    md = md_of("<pre><code>x<span>Y</span>z</code></pre>", false);
    CHECK_CONTAINS(md, "xYz");
    free(md);

    /* backticks inside inline code use a longer delimiter */
    md = md_of("<p><code>a`b</code></p>", false);
    CHECK_CONTAINS(md, "``a`b``");
    free(md);

    /* a fence inside <pre> cannot break out of the code block */
    md = md_of("<pre>before\n```\nafter</pre>", false);
    CHECK_CONTAINS(md, "````\nbefore");
    free(md);

    /* regression: thousands of children under <code>/<pre> (used to
     * overflow a fixed-size stack array) */
    {
        rbl_buf big;
        rbl_buf_init(&big);
        rbl_buf_append_str(&big, "<p><code>");
        for (int i = 0; i < 5000; i++) rbl_buf_append_str(&big, "<b>x</b>");
        rbl_buf_append_str(&big, "</code></p><pre>");
        for (int i = 0; i < 5000; i++) rbl_buf_append_str(&big, "<i>y</i>");
        rbl_buf_append_str(&big, "</pre>");
        md = md_of(rbl_buf_cstr(&big), false);
        CHECK(md != NULL && strlen(md) > 10000);
        free(md);
        rbl_buf_free(&big);
    }

    /* regression: <script> at the nesting cap must not leak into output */
    {
        rbl_buf deep;
        rbl_buf_init(&deep);
        for (int i = 0; i < 600; i++) rbl_buf_append_str(&deep, "<div>");
        rbl_buf_append_str(&deep, "<script>SECRETJS()</script>visible");
        md = md_of(rbl_buf_cstr(&deep), false);
        CHECK(md && strstr(md, "SECRETJS") == NULL);
        CHECK_CONTAINS(md, "visible");
        free(md);
        rbl_buf_free(&deep);
    }
}
