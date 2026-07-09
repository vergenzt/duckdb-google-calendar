#include <fstream>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <random>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <cctype>
#include <cstdio>
#include "json.hpp"

#define CPPHTTPLIB_OPENSSL_SUPPORT
#include "httplib.hpp"
#include <openssl/sha.h>
#include <openssl/rand.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#endif

#include "duckdb/common/exception/binder_exception.hpp"

#include "calendar_auth.hpp"
#include "calendar/util/encoding.hpp"
#include "calendar/util/options.hpp"
#include "calendar/util/query.hpp"

using json = nlohmann::json;

namespace duckdb {

// OAuth state nonce generator (the reference util helper was not ported).
static std::string GenerateRandomString(size_t length) {
	static const char charset[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
	std::random_device rd;
	std::mt19937 gen(rd());
	std::uniform_int_distribution<size_t> dist(0, sizeof(charset) - 2);
	std::string result;
	result.reserve(length);
	for (size_t i = 0; i < length; i++) {
		result += charset[dist(gen)];
	}
	return result;
}

// Generate a PKCE code verifier (RFC 7636): a high-entropy URL-safe string.
static std::string GeneratePkceVerifier() {
	unsigned char buf[32];
	if (RAND_bytes(buf, sizeof(buf)) != 1) {
		throw IOException("Failed to generate PKCE code verifier (RAND_bytes failed)");
	}
	return duckdb::gcal::Base64UrlEncode(buf, sizeof(buf));
}

// Derive the S256 code challenge from a PKCE verifier: base64url(SHA256(verifier)).
static std::string PkceChallengeS256(const std::string &verifier) {
	unsigned char digest[SHA256_DIGEST_LENGTH];
	SHA256(reinterpret_cast<const unsigned char *>(verifier.data()), verifier.size(), digest);
	return duckdb::gcal::Base64UrlEncode(digest, sizeof(digest));
}

static void CopySecret(const std::string &key, const CreateSecretInput &input, KeyValueSecret &result) {
	auto val = input.options.find(key);
	if (val != input.options.end()) {
		result.secret_map[key] = val->second;
	}
}

static void RegisterCommonSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["token"] = LogicalType::VARCHAR;
}

static unique_ptr<BaseSecret> CreateSecretFromAccessToken(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	CopySecret("token", input, *result);
	result->redact_keys.insert("token");
	return std::move(result);
}

static unique_ptr<BaseSecret> CreateSecretFromOAuth(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	string token = InitiateOAuthFlow();
	result->secret_map["token"] = token;
	result->redact_keys.insert("token");
	return std::move(result);
}

// Extract the service-account `client_email` and `private_key` from a Google key
// JSON document. `source` describes where the JSON came from, for error messages.
static void ParseServiceAccountKeyJson(const std::string &content, const std::string &source, std::string &email,
                                       std::string &secret) {
	json credentials;
	try {
		credentials = json::parse(content);
	} catch (const std::exception &e) {
		throw IOException("Could not parse Google service-account JSON from " + source + ": " + e.what());
	}
	if (!credentials.contains("client_email") || !credentials.contains("private_key")) {
		throw IOException("Google service-account JSON from " + source +
		                  " is missing required fields 'client_email' and/or 'private_key'");
	}
	email = credentials["client_email"].get<std::string>();
	secret = credentials["private_key"].get<std::string>();
}

static unique_ptr<BaseSecret> CreateSecretFromKeyFile(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);

	std::string email, secret;
	auto filepath = duckdb::gcal::GetStringOption(input.options, "filepath");
	auto key_env = duckdb::gcal::GetStringOption(input.options, "key_env");
	if (!filepath.empty()) {
		std::ifstream ifs(filepath);
		if (!ifs.is_open()) {
			throw IOException("Could not open JSON key file at: " + filepath);
		}
		std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
		ParseServiceAccountKeyJson(content, "file '" + filepath + "'", email, secret);
	} else if (!key_env.empty()) {
		// `key_env` names an environment variable whose value is the full key JSON.
		// The secret itself never appears in the SQL statement (or query history/logs).
		const char *raw = std::getenv(key_env.c_str());
		if (!raw || !*raw) {
			throw BinderException("Environment variable '" + key_env +
			                      "' named by key_env is not set or is empty");
		}
		ParseServiceAccountKeyJson(std::string(raw), "environment variable '" + key_env + "'", email, secret);
	} else {
		email = duckdb::gcal::GetStringOption(input.options, "email");
		if (email.empty()) {
			throw BinderException(
			    "key_file provider requires one of: 'filepath', 'key_env', or 'email' + 'secret'");
		}
		secret = duckdb::gcal::GetStringOption(input.options, "secret");
		if (secret.empty()) {
			throw BinderException("Must provide secret value if not using filepath or key_env");
		}
	}

	(*result).secret_map["email"] = Value(email);
	(*result).secret_map["secret"] = Value(secret);
	CopySecret("filepath", input, *result); // Store the filepath anyway

	result->redact_keys.insert("secret");
	result->redact_keys.insert("filepath");
	result->redact_keys.insert("token");

	return std::move(result);
}

void CreateGoogleCalendarSecretFunctions::Register(ExtensionLoader &loader) {
	string type = "google_calendar";

	SecretType secret_type;
	secret_type.name = type;
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "oauth";

	CreateSecretFunction access_token_function = {type, "access_token", CreateSecretFromAccessToken, {}};
	RegisterCommonSecretParameters(access_token_function);

	CreateSecretFunction oauth_function = {type, "oauth", CreateSecretFromOAuth, {}};
	RegisterCommonSecretParameters(oauth_function);

	CreateSecretFunction key_file_function = {type, "key_file", CreateSecretFromKeyFile, {}};
	key_file_function.named_parameters["filepath"] = LogicalType::VARCHAR;
	key_file_function.named_parameters["key_env"] = LogicalType::VARCHAR;
	key_file_function.named_parameters["email"] = LogicalType::VARCHAR;
	key_file_function.named_parameters["secret"] = LogicalType::VARCHAR;
	RegisterCommonSecretParameters(key_file_function);

	loader.RegisterSecretType(secret_type);
	loader.RegisterFunction(access_token_function);
	loader.RegisterFunction(oauth_function);
	loader.RegisterFunction(key_file_function);
}

// Best-effort launch of the system browser at `url`. Returns false on platforms
// where no display is available (e.g. headless Linux), so the caller can fall
// back to printing the URL for manual copy/paste.
static bool OpenInBrowser(const std::string &url) {
#ifdef __linux__
	const char *display = std::getenv("DISPLAY");
	const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
	if (!display && !wayland_display) {
		return false;
	}
#endif
#ifdef _WIN32
	system(("start \"\" \"" + url + "\"").c_str());
#elif __APPLE__
	system(("open \"" + url + "\"").c_str());
#elif __linux__
	system(("xdg-open \"" + url + "\"").c_str());
#endif
	return true;
}

// If GOOGLE_CALENDAR_OAUTH_REDIRECT_URI names a fixed loopback port
// (e.g. http://127.0.0.1:8085), return that port so we bind the local listener
// to it. Returns 0 to request an ephemeral port (the default; Google "Desktop"
// OAuth clients accept any loopback port without prior registration).
static int DesiredLoopbackPort() {
	const char *redirect_env = std::getenv("GOOGLE_CALENDAR_OAUTH_REDIRECT_URI");
	if (!redirect_env || !*redirect_env) {
		return 0;
	}
	const std::string uri = redirect_env;
	size_t host_pos = uri.find("://");
	if (host_pos == std::string::npos) {
		return 0;
	}
	size_t colon = uri.find(':', host_pos + 3);
	if (colon == std::string::npos) {
		return 0;
	}
	size_t end = colon + 1;
	while (end < uri.size() && std::isdigit(static_cast<unsigned char>(uri[end]))) {
		end++;
	}
	if (end == colon + 1) {
		return 0;
	}
	try {
		return std::stoi(uri.substr(colon + 1, end - colon - 1));
	} catch (...) {
		return 0;
	}
}

// Service name under which the Google refresh token is filed in the OS keyring.
// The account is the OAuth client_id, so distinct OAuth apps don't collide.
static const std::string KEYCHAIN_SERVICE = "duckdb-google-calendar";

#ifdef __APPLE__
// Read a secret from the macOS login keychain. Returns false if absent.
static bool KeychainGet(const std::string &service, const std::string &account, std::string &out) {
	CFStringRef svc = CFStringCreateWithCString(nullptr, service.c_str(), kCFStringEncodingUTF8);
	CFStringRef acc = CFStringCreateWithCString(nullptr, account.c_str(), kCFStringEncodingUTF8);
	const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecReturnData, kSecMatchLimit};
	const void *vals[] = {kSecClassGenericPassword, svc, acc, kCFBooleanTrue, kSecMatchLimitOne};
	CFDictionaryRef query =
	    CFDictionaryCreate(nullptr, keys, vals, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDataRef data = nullptr;
	OSStatus status = SecItemCopyMatching(query, reinterpret_cast<CFTypeRef *>(&data));
	CFRelease(query);
	CFRelease(svc);
	CFRelease(acc);
	if (status != errSecSuccess || !data) {
		return false;
	}
	out.assign(reinterpret_cast<const char *>(CFDataGetBytePtr(data)), CFDataGetLength(data));
	CFRelease(data);
	return true;
}

// Store (or replace) a secret in the macOS login keychain. Best-effort: a failure
// here just means we don't cache, so we log and carry on rather than fail the run.
static void KeychainSet(const std::string &service, const std::string &account, const std::string &value) {
	CFStringRef svc = CFStringCreateWithCString(nullptr, service.c_str(), kCFStringEncodingUTF8);
	CFStringRef acc = CFStringCreateWithCString(nullptr, account.c_str(), kCFStringEncodingUTF8);
	CFDataRef val = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(value.data()), value.size());
	{
		const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount};
		const void *vals[] = {kSecClassGenericPassword, svc, acc};
		CFDictionaryRef del = CFDictionaryCreate(nullptr, keys, vals, 3, &kCFTypeDictionaryKeyCallBacks,
		                                         &kCFTypeDictionaryValueCallBacks);
		SecItemDelete(del);
		CFRelease(del);
	}
	const void *keys[] = {kSecClass, kSecAttrService, kSecAttrAccount, kSecValueData};
	const void *vals[] = {kSecClassGenericPassword, svc, acc, val};
	CFDictionaryRef add =
	    CFDictionaryCreate(nullptr, keys, vals, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus status = SecItemAdd(add, nullptr);
	CFRelease(add);
	CFRelease(val);
	CFRelease(svc);
	CFRelease(acc);
	if (status != errSecSuccess) {
		std::cerr << "Warning: could not save Google refresh token to keychain (OSStatus " << status
		          << "); next run will re-authorize.\n";
	}
}
#else
// ponytail: keyring caching is macOS-only; elsewhere every run does the browser flow.
// Add libsecret (Linux) / wincred (Windows) backends here if that need shows up.
static bool KeychainGet(const std::string &, const std::string &, std::string &) {
	return false;
}
static void KeychainSet(const std::string &, const std::string &, const std::string &) {
}
#endif

// Exchange a stored refresh token for a fresh access token (no browser, no user
// interaction). Throws if the refresh token is revoked/expired so the caller can
// fall back to the interactive flow.
static std::string RefreshAccessToken(const std::string &client_id, const std::string &client_secret,
                                      const std::string &refresh_token) {
	duckdb_httplib_openssl::Client token_client("https://oauth2.googleapis.com");
	duckdb_httplib_openssl::Params params;
	params.emplace("client_id", client_id);
	if (!client_secret.empty()) {
		params.emplace("client_secret", client_secret);
	}
	params.emplace("refresh_token", refresh_token);
	params.emplace("grant_type", "refresh_token");

	auto result = token_client.Post("/token", params);
	if (!result) {
		throw IOException("OAuth token refresh request failed: " +
		                  duckdb_httplib_openssl::to_string(result.error()));
	}
	if (result->status != 200) {
		throw IOException("OAuth token refresh failed (HTTP " + std::to_string(result->status) + "): " +
		                  result->body);
	}
	json resp = json::parse(result->body);
	if (!resp.contains("access_token")) {
		throw IOException("OAuth token refresh response did not contain an access_token: " + result->body);
	}
	return resp["access_token"].get<std::string>();
}

// OAuth 2.0 authorization-code flow with PKCE for installed/desktop apps
// (https://developers.google.com/identity/protocols/oauth2/native-app).
// Replaces the deprecated OOB flow: we open the consent screen, capture the
// authorization code on a loopback HTTP listener, then exchange it (with the
// PKCE verifier) for an access token at the token endpoint.
std::string InitiateOAuthFlow() {
	const char *client_id_env = std::getenv("GOOGLE_CALENDAR_OAUTH_CLIENT_ID");
	if (!client_id_env || !*client_id_env) {
		throw BinderException(
		    "The 'oauth' provider requires a registered Google OAuth client. Set GOOGLE_CALENDAR_OAUTH_CLIENT_ID "
		    "(and GOOGLE_CALENDAR_OAUTH_CLIENT_SECRET for Desktop-app clients), or use the 'key_file' / "
		    "'access_token' provider.");
	}
	const std::string client_id = client_id_env;
	const char *client_secret_env = std::getenv("GOOGLE_CALENDAR_OAUTH_CLIENT_SECRET");
	const std::string client_secret = (client_secret_env && *client_secret_env) ? client_secret_env : "";

	// Fast path: if we cached a refresh token from a previous run, mint a new
	// access token silently. Delete the keychain item to force re-authorization:
	//   security delete-generic-password -s duckdb-google-calendar
	std::string cached_refresh;
	if (KeychainGet(KEYCHAIN_SERVICE, client_id, cached_refresh) && !cached_refresh.empty()) {
		try {
			return RefreshAccessToken(client_id, client_secret, cached_refresh);
		} catch (const std::exception &e) {
			std::cerr << "Cached Google refresh token unusable (" << e.what()
			          << "); falling back to browser authorization.\n";
		}
	}

	// PKCE parameters and CSRF state.
	const std::string code_verifier = GeneratePkceVerifier();
	const std::string code_challenge = PkceChallengeS256(code_verifier);
	const std::string state = GenerateRandomString(32);

	// Stand up a loopback listener to receive Google's redirect.
	duckdb_httplib_openssl::Server server;
	std::mutex mtx;
	std::condition_variable cv;
	bool done = false;
	std::string captured_code;
	std::string captured_state;
	std::string captured_error;

	server.Get("/", [&](const duckdb_httplib_openssl::Request &req, duckdb_httplib_openssl::Response &res) {
		{
			std::lock_guard<std::mutex> lock(mtx);
			if (req.has_param("error")) {
				captured_error = req.get_param_value("error");
			}
			if (req.has_param("code")) {
				captured_code = req.get_param_value("code");
			}
			if (req.has_param("state")) {
				captured_state = req.get_param_value("state");
			}
			done = true;
		}
		cv.notify_one();
		const bool ok = captured_error.empty() && !captured_code.empty();
		res.set_content(std::string("<!doctype html><html><body style=\"font-family:sans-serif\"><h2>") +
		                    (ok ? "Authorization complete" : "Authorization failed") +
		                    "</h2><p>You can close this tab and return to DuckDB.</p></body></html>",
		                "text/html");
	});

	const int desired_port = DesiredLoopbackPort();
	int port = desired_port;
	if (desired_port != 0) {
		if (!server.bind_to_port("127.0.0.1", desired_port)) {
			throw IOException("Could not bind OAuth loopback listener to 127.0.0.1:" + std::to_string(desired_port) +
			                  " (set by GOOGLE_CALENDAR_OAUTH_REDIRECT_URI)");
		}
	} else {
		port = server.bind_to_any_port("127.0.0.1");
		if (port < 0) {
			throw IOException("Could not bind OAuth loopback listener to 127.0.0.1");
		}
	}

	std::thread listener([&]() { server.listen_after_bind(); });
	server.wait_until_ready();

	const std::string redirect_uri = "http://127.0.0.1:" + std::to_string(port);
	const std::string scope = "https://www.googleapis.com/auth/calendar";
	const std::string auth_request_url =
	    "https://accounts.google.com/o/oauth2/v2/auth?response_type=code"
	    "&client_id=" +
	    gcal::UrlEncode(client_id) + "&redirect_uri=" + gcal::UrlEncode(redirect_uri) + "&scope=" +
	    gcal::UrlEncode(scope) + "&state=" + gcal::UrlEncode(state) + "&code_challenge=" + gcal::UrlEncode(code_challenge) +
	    "&code_challenge_method=S256&access_type=offline&prompt=consent";

	std::cout << "Visit the below URL to authorize DuckDB Google Calendar\n";
	std::cout << auth_request_url << '\n';
	OpenInBrowser(auth_request_url);
	std::cout << "Waiting for authorization in your browser...\n";

	// Wait for the redirect (with a timeout so a stuck flow doesn't hang DuckDB).
	{
		std::unique_lock<std::mutex> lock(mtx);
		if (!cv.wait_for(lock, std::chrono::seconds(300), [&]() { return done; })) {
			server.stop();
			if (listener.joinable()) {
				listener.join();
			}
			throw IOException("Timed out waiting for Google OAuth authorization (no redirect received in 5 minutes)");
		}
	}
	server.stop();
	if (listener.joinable()) {
		listener.join();
	}

	if (!captured_error.empty()) {
		throw IOException("Google OAuth authorization failed: " + captured_error);
	}
	if (captured_state != state) {
		throw IOException("Google OAuth state mismatch (possible CSRF); aborting.");
	}
	if (captured_code.empty()) {
		throw IOException("Google OAuth authorization did not return a code.");
	}

	// Exchange the authorization code for an access token.
	duckdb_httplib_openssl::Client token_client("https://oauth2.googleapis.com");
	duckdb_httplib_openssl::Params params;
	params.emplace("code", captured_code);
	params.emplace("client_id", client_id);
	if (!client_secret.empty()) {
		params.emplace("client_secret", client_secret);
	}
	params.emplace("code_verifier", code_verifier);
	params.emplace("grant_type", "authorization_code");
	params.emplace("redirect_uri", redirect_uri);

	auto result = token_client.Post("/token", params);
	if (!result) {
		throw IOException("OAuth token exchange request failed: " +
		                  duckdb_httplib_openssl::to_string(result.error()));
	}
	if (result->status != 200) {
		throw IOException("OAuth token exchange failed (HTTP " + std::to_string(result->status) + "): " +
		                  result->body);
	}

	json token_response;
	try {
		token_response = json::parse(result->body);
	} catch (const std::exception &e) {
		throw IOException(std::string("Could not parse OAuth token response: ") + e.what());
	}
	if (!token_response.contains("access_token")) {
		throw IOException("OAuth token response did not contain an access_token: " + result->body);
	}
	// Cache the refresh token so subsequent runs skip the browser. `access_type=offline`
	// + `prompt=consent` above guarantee Google returns one on the interactive flow.
	if (token_response.contains("refresh_token")) {
		KeychainSet(KEYCHAIN_SERVICE, client_id, token_response["refresh_token"].get<std::string>());
	}
	return token_response["access_token"].get<std::string>();
}

} // namespace duckdb
