---
title: "HTTP Requests"
description: "Perform HTTP GET requests from the command line."
weight: 20
---

Make HTTP requests to web servers and APIs.

## Basic HTTP GET

Fetch content from a URL:

```
ethhttp https://<url> <lines_to_show|all>
```

**Parameters**:
- `<url>` — The full URL (supports http:// and https://)
- `<lines_to_show>` — Number of response lines to display, or `all` for complete response

**Examples**:

Fetch the first 10 lines of a webpage:
```
ethhttp https://example.com 10
```

Fetch the entire response:
```
ethhttp https://example.com all
```

Fetch from a specific path:
```
ethhttp https://api.example.com/status 5
```

**Use Cases**:
- **API Testing** — Query REST APIs
- **Web Scraping** — Retrieve website content
- **Service Verification** — Check if a service is responding
- **Configuration** — Fetch remote config files

## Troubleshooting

- **Connection refused** — Ensure the server is running and accessible from your network
- **Timeout** — The server may be unreachable or slow to respond
- **SSL/TLS errors** — Some servers may require specific certificates; try http:// instead of https://
