package rebellion.example

import rebellion.client.Action
import rebellion.client.CrawlRequest
import rebellion.client.RebellionClient
import rebellion.client.RebellionException
import rebellion.client.ScrapeOptions
import rebellion.client.ScrapeRequest

/**
 * Usage: ./gradlew run --args="<url> [baseUrl]"
 * The API key, if the server requires one, is read from REBELLION_API_KEY.
 */
fun main(args: Array<String>) {
    val url = args.getOrNull(0) ?: "https://example.com/"
    val base = args.getOrNull(1) ?: System.getenv("REBELLION_URL") ?: "http://127.0.0.1:8080"
    val client = RebellionClient(base, apiKey = System.getenv("REBELLION_API_KEY"))

    val health = client.health()
    println("server ${health.version}, browser available: ${health.browser}")

    // 1) Plain scrape: Markdown + metadata.
    val doc = client.scrape(ScrapeRequest(url, engine = "auto"))
    println("scrape: ${doc.statusCode} ${doc.engine} \"${doc.title}\" lang=${doc.language} " +
        "markdown=${doc.markdown.length} chars, ${doc.links.size} links")
    println(doc.markdown.lineSequence().take(5).joinToString("\n") { "  | $it" })

    // 2) Map: discover URLs (sitemaps + homepage links).
    val urls = client.map(url, limit = 50)
    println("map: ${urls.size} URLs, e.g. ${urls.take(3)}")

    // 3) Small crawl over the HTTP engine.
    val pages = client.crawl(CrawlRequest(url, limit = 3, depth = 1, scrapeOptions = ScrapeOptions(engine = "http")))
    println("crawl: ${pages.size} pages: ${pages.map { it.url }}")

    // 4) Browser actions (needs Chromium on the server). The cookie-banner
    //    click is optional so a missing banner does not fail the request.
    if (health.browser) {
        val shot = client.scrape(
            ScrapeRequest(
                url,
                actions = listOf(Action.click("#accept-cookies", optional = true), Action.scrollBottom(), Action.waitMs(300)),
                screenshot = true,
            ),
        )
        println("actions: ${shot.engine}, screenshot ${shot.screenshot?.length ?: 0} chars of data URL")
    }

    // 5) Errors carry a machine-readable code ("bad_url", "blocked",
    //    "robots_denied", "timeout", "busy", ...).
    try {
        client.scrape(ScrapeRequest("file:///etc/passwd"))
    } catch (e: RebellionException) {
        println("error handling: HTTP ${e.httpStatus}, code=${e.code}")
    }
}
