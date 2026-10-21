/* Main-content extraction: what survives into the Markdown and what is
 * dropped as page chrome. Fixtures are shaped like German bank product
 * pages, where losing the headline or the offer line is the costly mistake. */
#include "test.h"
#include "html.h"
#include "extract.h"

#include <stdlib.h>
#include <string.h>

#define BODY_TEXT                                                                      \
    "Mit Festgeld Plus legen Sie Ihr Geld fuer eine feste Laufzeit zu einem "          \
    "garantierten Zinssatz an. Die Einlagen sind ueber die gesetzliche "               \
    "Einlagensicherung bis 100.000 Euro je Kunde geschuetzt. Sie waehlen eine "        \
    "Laufzeit zwischen 6 und 36 Monaten und erhalten die Zinsen am Laufzeitende."

static char *main_md(const char *html) {
    rbl_node *root = rbl_html_parse(html, strlen(html));
    if (!root) return NULL;
    char *md = rbl_main_content_markdown(root, "https://bank.example/festgeld");
    rbl_node_free(root);
    return md;
}

static size_t count(const char *hay, const char *needle) {
    size_t n = 0;
    for (const char *p = hay; p && (p = strstr(p, needle)) != NULL; p += strlen(needle))
        n++;
    return n;
}

#define CHECK_ABSENT(hay, needle) CHECK((hay) && strstr((hay), (needle)) == NULL)
#define CHECK_BEFORE(hay, a, b)                                                        \
    CHECK((hay) && strstr((hay), (a)) && strstr((hay), (b)) &&                         \
          strstr((hay), (a)) < strstr((hay), (b)))

/* A <header> inside <main> introduces the content; the site header and
 * footer around it are chrome. A <footer> inside the content (footnotes)
 * stays. */
static void header_inside_main(void) {
    char *md = main_md(
        "<html><body><header class=site><a href=/>Logo</a><nav><a "
        "href=/p>Privatkunden</a>"
        "</nav></header><main><section class=hero><header><h1>Festgeld Plus</h1>"
        "<h2>Jetzt 3,00 % p. a. Zinsen sichern.</h2><p>Sichern Sie sich jetzt "
        "attraktive "
        "Zinsen.</p></header><img src=/hero.jpg "
        "alt=Sparschwein></section><div><p>" BODY_TEXT
        "</p><footer><p>Zinssatz gilt fuer Neuanlagen ab 5.000 Euro.</p></footer></div>"
        "</main><footer class=site>Impressum Datenschutz</footer></body></html>");
    CHECK_CONTAINS(md, "# Festgeld Plus");
    CHECK_CONTAINS(md, "## Jetzt 3,00 % p. a. Zinsen sichern.");
    CHECK_CONTAINS(md, "Sichern Sie sich jetzt attraktive Zinsen.");
    CHECK_CONTAINS(md, "Zinssatz gilt fuer Neuanlagen ab 5.000 Euro.");
    CHECK_CONTAINS(md, "![Sparschwein]");
    CHECK_ABSENT(md, "Logo");
    CHECK_ABSENT(md, "Privatkunden");
    CHECK_ABSENT(md, "Impressum");
    free(md);
}

/* The hero sits next to the text block with a link list in between, so the
 * text block alone scores best: the headline block is put in front of it,
 * without the links. */
static void hero_beside_content(void) {
    char links[2048] = "";
    for (int i = 0; i < 25; i++)
        strcat(links, "<a href=/p>Weitere Produktinformation</a> ");
    char html[8192];
    snprintf(html, sizeof(html),
             "<html><body><div id=wrap><div class=hero><h1>Festgeld Plus</h1>"
             "<h2>Jetzt 3,00 %% p. a. Zinsen sichern.</h2><p>Kurzer Einstieg.</p></div>"
             "<div class=links>%s</div><div class=text><p>%s</p><p>%s</p></div></div>"
             "</body></html>",
             links, BODY_TEXT, BODY_TEXT);
    char *md = main_md(html);
    CHECK_CONTAINS(md, "# Festgeld Plus");
    CHECK_CONTAINS(md, "## Jetzt 3,00 % p. a. Zinsen sichern.");
    CHECK_CONTAINS(md, "Kurzer Einstieg.");
    CHECK_BEFORE(md, "# Festgeld Plus", "Mit Festgeld Plus legen");
    CHECK_ABSENT(md, "Weitere Produktinformation");
    free(md);
}

/* A hero <header> at page level (outside <main>) still provides the headline. */
static void page_level_hero_header(void) {
    char *md = main_md("<html><body><header class=hero><h1>Festgeld Plus</h1><p>Jetzt "
                       "3,00 % p. a. sichern.</p></header><main><p>" BODY_TEXT
                       "</p><p>" BODY_TEXT "</p></main><footer>Impressum</footer>"
                       "</body></html>");
    CHECK_CONTAINS(md, "# Festgeld Plus");
    CHECK_CONTAINS(md, "Jetzt 3,00 % p. a. sichern.");
    CHECK_BEFORE(md, "# Festgeld Plus", "Mit Festgeld Plus legen");
    CHECK_ABSENT(md, "Impressum");
    free(md);
}

/* A linked logo in an <h1>, or the site name in the banner, is not the
 * page's headline when the content has its own. */
static void logo_and_site_name_stay_out(void) {
    char *md = main_md(
        "<html><body><header><h1><a href=/>Bankname</a></h1><nav><a "
        "href=/x>Menue</a></nav></header><main><article><h2>Tagesgeld</h2><p>" BODY_TEXT
        "</p></article></main></body></html>");
    CHECK_CONTAINS(md, "## Tagesgeld");
    CHECK_ABSENT(md, "Bankname");
    CHECK_ABSENT(md, "Menue");
    free(md);

    md = main_md("<html><body><header><h1>Musterbank eG</h1><nav><a href=/x>Menue</a>"
                 "</nav></header><main><h1>Festgeld Plus</h1><p>" BODY_TEXT
                 "</p></main></body></html>");
    CHECK_CONTAINS(md, "# Festgeld Plus");
    CHECK_ABSENT(md, "Musterbank eG");
    CHECK(count(md, "# Festgeld Plus") == 1); /* not duplicated */
    free(md);
}

/* ARIA landmarks mark chrome regardless of the element used. */
static void aria_landmarks(void) {
    char *md = main_md(
        "<html><body><div role=banner>Kopfzeile</div><div role=navigation>"
        "<a href=/a>Navigation</a></div><main><h1>Festgeld Plus</h1><p>" BODY_TEXT
        "</p><header role=banner>Banner im Inhalt</header></main>"
        "<div role=contentinfo>Fusszeile</div></body></html>");
    CHECK_CONTAINS(md, "# Festgeld Plus");
    CHECK_ABSENT(md, "Kopfzeile");
    CHECK_ABSENT(md, "Navigation");
    CHECK_ABSENT(md, "Banner im Inhalt");
    CHECK_ABSENT(md, "Fusszeile");
    free(md);
}

static void page_chrome_rule(void) {
    const char *html = "<body><header id=a></header><main><header id=b></header></main>"
                       "<article><footer id=c></footer></article><footer id=d></footer>"
                       "<div><nav id=e></nav></div></body>";
    rbl_node *root = rbl_html_parse(html, strlen(html));
    rbl_node *body = root;
    while (body && !rbl_node_is(body, "body")) body = body->first_child;
    CHECK(body != NULL);
    if (!body) {
        rbl_node_free(root);
        return;
    }
    rbl_node *a = body->first_child;      /* page header */
    rbl_node *main_el = a->next;          /* main > header */
    rbl_node *article = main_el->next;    /* article > footer */
    rbl_node *d = article->next;          /* page footer */
    rbl_node *nav = d->next->first_child; /* div > nav */
    CHECK(rbl_node_is_page_chrome(a));
    CHECK(!rbl_node_is_page_chrome(main_el->first_child));
    CHECK(!rbl_node_is_page_chrome(article->first_child));
    CHECK(rbl_node_is_page_chrome(d));
    CHECK(rbl_node_is_page_chrome(nav));
    CHECK(!rbl_node_is_page_chrome(main_el));
    rbl_node_free(root);
}

void test_extract(void) {
    header_inside_main();
    hero_beside_content();
    page_level_hero_header();
    logo_and_site_name_stay_out();
    aria_landmarks();
    page_chrome_rule();
}
