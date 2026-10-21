package rebellion.client

import kotlinx.serialization.KSerializer
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.net.URI
import java.net.http.HttpClient
import java.net.http.HttpRequest
import java.net.http.HttpResponse
import java.time.Duration

/** A browser action run before the page is captured (forces the browser engine). */
@Serializable
data class Action(
    val type: String, // click | fill | submit | wait_for | scroll | scroll_bottom | wait
    val selector: String? = null,
    val value: String? = null, // fill
    val ms: Long? = null, // wait, wait_for
    val x: Int? = null, // scroll
    val y: Int? = null, // scroll
    val optional: Boolean? = null, // don't fail the request if this action fails
) {
    companion object {
        fun click(selector: String, optional: Boolean? = null) =
            Action("click", selector, optional = optional)
        fun fill(selector: String, value: String) = Action("fill", selector, value = value)
        fun submit(selector: String) = Action("submit", selector)
        fun waitFor(selector: String, ms: Long? = null) = Action("wait_for", selector, ms = ms)
        fun scrollBottom() = Action("scroll_bottom")
        fun waitMs(ms: Long) = Action("wait", ms = ms)
    }
}

@Serializable
data class ScrapeRequest(
    val url: String,
    val engine: String? = null, // auto (default) | http | browser
    val onlyMainContent: Boolean? = null,
    val includeHtml: Boolean? = null,
    val includeLinks: Boolean? = null,
    val respectRobots: Boolean? = null,
    val timeoutMs: Long? = null,
    val waitAfterLoadMs: Long? = null,
    val userAgent: String? = null,
    val acceptLanguage: String? = null,
    val screenshot: Boolean? = null,
    val fullPage: Boolean? = null,
    val actions: List<Action>? = null,
)

@Serializable
data class ScrapeOptions(
    val engine: String? = null,
    val onlyMainContent: Boolean? = null,
    val includeHtml: Boolean? = null,
    val includeLinks: Boolean? = null,
    val respectRobots: Boolean? = null,
    val timeoutMs: Long? = null,
    val waitAfterLoadMs: Long? = null,
    val userAgent: String? = null,
    val acceptLanguage: String? = null,
)

@Serializable
data class CrawlRequest(
    val url: String,
    val limit: Int? = null,
    val depth: Int? = null,
    val sameHostOnly: Boolean? = null,
    val allowSubdomains: Boolean? = null,
    val include: String? = null, // POSIX ERE on the full URL
    val exclude: String? = null,
    val scrapeOptions: ScrapeOptions? = null,
)

@Serializable
data class MapRequest(val url: String, val limit: Int? = null)

/** A scraped page. */
@Serializable
data class Document(
    val url: String,
    val statusCode: Int,
    val engine: String,
    val title: String? = null,
    val description: String? = null,
    val language: String? = null,
    val markdown: String = "",
    val html: String? = null,
    val links: List<String> = emptyList(),
    val screenshot: String? = null, // data:image/png;base64,...
)

@Serializable
data class Health(val status: String, val service: String, val version: String, val browser: Boolean)

@Serializable
private data class ScrapeResponse(val success: Boolean, val data: Document? = null)

@Serializable
private data class CrawlResponse(val success: Boolean, val data: List<Document> = emptyList())

@Serializable
private data class MapResponse(val success: Boolean, val links: List<String> = emptyList())

@Serializable
private data class ErrorResponse(val success: Boolean = false, val code: String = "error", val error: String = "")

/**
 * Thrown for any non-2xx answer. [code] is the machine-readable error code, e.g.
 * "blocked", "robots_denied", "timeout", "network", "busy", "action_failed".
 */
class RebellionException(val httpStatus: Int, val code: String, message: String) :
    RuntimeException("$code ($httpStatus): $message")

/** Minimal client for rebellion-server. Thread-safe; share one instance. */
class RebellionClient(
    baseUrl: String,
    private val apiKey: String? = null,
    private val requestTimeout: Duration = Duration.ofMinutes(5),
    private val http: HttpClient = HttpClient.newBuilder().connectTimeout(Duration.ofSeconds(10)).build(),
) {
    private val base = baseUrl.trimEnd('/')
    private val json = Json {
        ignoreUnknownKeys = true // forward-compatible with new fields
        explicitNulls = false // omit unset options
    }

    fun health(): Health = json.decodeFromString(Health.serializer(), send("GET", "/health", null))

    fun scrape(request: ScrapeRequest): Document =
        post("/v1/scrape", request, ScrapeRequest.serializer(), ScrapeResponse.serializer()).data
            ?: throw RebellionException(200, "bad_response", "no data in response")

    fun scrape(url: String): Document = scrape(ScrapeRequest(url))

    fun crawl(request: CrawlRequest): List<Document> =
        post("/v1/crawl", request, CrawlRequest.serializer(), CrawlResponse.serializer()).data

    fun map(url: String, limit: Int? = null): List<String> =
        post("/v1/map", MapRequest(url, limit), MapRequest.serializer(), MapResponse.serializer()).links

    private fun <Req, Resp> post(path: String, body: Req, reqSer: KSerializer<Req>, respSer: KSerializer<Resp>): Resp =
        json.decodeFromString(respSer, send("POST", path, json.encodeToString(reqSer, body)))

    private fun send(method: String, path: String, body: String?): String {
        val builder = HttpRequest.newBuilder(URI.create(base + path)).timeout(requestTimeout)
        if (apiKey != null) builder.header("Authorization", "Bearer $apiKey")
        if (body != null) {
            builder.header("Content-Type", "application/json")
            builder.method(method, HttpRequest.BodyPublishers.ofString(body))
        } else {
            builder.method(method, HttpRequest.BodyPublishers.noBody())
        }
        val resp = http.send(builder.build(), HttpResponse.BodyHandlers.ofString())
        if (resp.statusCode() !in 200..299) {
            val err = runCatching { json.decodeFromString(ErrorResponse.serializer(), resp.body()) }
                .getOrDefault(ErrorResponse(error = resp.body().take(200)))
            throw RebellionException(resp.statusCode(), err.code, err.error)
        }
        return resp.body()
    }
}
