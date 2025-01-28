# Rocket Browser

Rocket Browser is a simple web browser built using GTK and WebKit2GTK. It provides basic browsing features like back, forward, search, home, and supports loading local servers running on `localhost` with specified ports.

## Updated Features
- **New Tab**: Users can open new tabs within the same browser window.
- **Close Tab**: Users can close tabs individually.
- **Switch Tabs**: Users can switch between open tabs seamlessly.
- **Request/Response Interception**: Monitor and analyze HTTP requests and responses with detailed information.
  
## Features

### Basic Navigation
- **Navigation**: Supports forward, backward navigation using buttons.
- **Search**: Allows searching via Google, Bing, DuckDuckGo, and Yahoo.
- **Home Button**: Set to load Google as the default homepage.
- **URL/Domain Handling**: If the input is a valid URL, it will load directly. If the input is a domain, it will be prefixed with `http://` and loaded.
- **Localhost Support**: Can access local servers with `localhost` or `127.0.0.1` followed by a port number (e.g., `localhost:8080`).

### HTTP Traffic Inspection
- **Request Interception**: View and analyze HTTP requests before they are sent.
- **Response Analysis**: Examine server responses including status codes and headers.
- **Headers Inspection**: View both request and response headers in detail.
- **Forward/Drop Requests**: Control request flow by choosing to forward or drop intercepted requests.
- **Traffic Monitoring**: Toggle interception on/off with a dedicated button.

## Requirements

To compile and run the Rocket Browser, ensure you have the following dependencies installed:

- **GTK 3.0**: For GUI elements.
- **WebKit2GTK**: For rendering web pages and HTTP traffic inspection.
- **SQLite3**: For storage support (if required).
- **OpenSSL**: For secure HTTP connections.
- **libcurl**: For handling URL fetching.
- **Glib**: For general utility functions.

## Installation

### Prerequisites

Make sure the following libraries are installed on your system:

```sh
# For Ubuntu/Debian systems:
sudo apt-get install libgtk-3-dev libwebkit2gtk-4.0-dev libsqlite3-dev libssl-dev libcurl4-openssl-dev