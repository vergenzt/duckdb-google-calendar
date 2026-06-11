#include <fstream>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <random>
#include "json.hpp"

#include "duckdb/common/exception/binder_exception.hpp"

#include "calendar_auth.hpp"
#include "calendar/util/options.hpp"

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

static void CopySecret(const std::string &key, const CreateSecretInput &input, KeyValueSecret &result) {
	auto val = input.options.find(key);
	if (val != input.options.end()) {
		result.secret_map[key] = val->second;
	}
}

static void RegisterCommonSecretParameters(CreateSecretFunction &function) {
	function.named_parameters["token"] = LogicalType::VARCHAR;
}

static void RedactCommonKeys(KeyValueSecret &result) {
	result.redact_keys.insert("proxy_password");
}

static unique_ptr<BaseSecret> CreateSecretFromAccessToken(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	CopySecret("token", input, *result);
	RedactCommonKeys(*result);
	result->redact_keys.insert("token");
	return std::move(result);
}

static unique_ptr<BaseSecret> CreateSecretFromOAuth(ClientContext &context, CreateSecretInput &input) {
	auto scope = input.scope;
	auto result = make_uniq<KeyValueSecret>(scope, input.type, input.provider, input.name);
	string token = InitiateOAuthFlow();
	result->secret_map["token"] = token;
	RedactCommonKeys(*result);
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

	RedactCommonKeys(*result);
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
	access_token_function.named_parameters["access_token"] = LogicalType::VARCHAR;
	RegisterCommonSecretParameters(access_token_function);

	CreateSecretFunction oauth_function = {type, "oauth", CreateSecretFromOAuth, {}};
	oauth_function.named_parameters["use_oauth"] = LogicalType::BOOLEAN;
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

std::string InitiateOAuthFlow() {
	const char *client_id_env = std::getenv("GOOGLE_CALENDAR_OAUTH_CLIENT_ID");
	if (!client_id_env || !*client_id_env) {
		throw BinderException(
		    "The 'oauth' provider requires a registered Google OAuth client. Set GOOGLE_CALENDAR_OAUTH_CLIENT_ID "
		    "(and optionally GOOGLE_CALENDAR_OAUTH_REDIRECT_URI), or use the 'key_file' / 'access_token' provider.");
	}
	const char *redirect_env = std::getenv("GOOGLE_CALENDAR_OAUTH_REDIRECT_URI");
	const std::string client_id = client_id_env;
	const std::string redirect_uri = (redirect_env && *redirect_env) ? redirect_env : "urn:ietf:wg:oauth:2.0:oob";
	const std::string auth_url = "https://accounts.google.com/o/oauth2/v2/auth";

	std::string state = GenerateRandomString(10);
	std::string auth_request_url = auth_url + "?client_id=" + client_id + "&redirect_uri=" + redirect_uri +
	                               "&response_type=token" + "&scope=https://www.googleapis.com/auth/calendar" +
	                               "&state=" + state;

	std::cout << "Visit the below URL to authorize DuckDB Google Calendar" << '\n';
	std::cout << auth_request_url << '\n';

	bool should_open_browser = true;
#ifdef __linux__
	const char *display = std::getenv("DISPLAY");
	const char *wayland_display = std::getenv("WAYLAND_DISPLAY");
	if (!display && !wayland_display) {
		should_open_browser = false;
	}
#endif
	if (should_open_browser) {
#ifdef _WIN32
		system(("start \"\" \"" + auth_request_url + "\"").c_str());
#elif __APPLE__
		system(("open \"" + auth_request_url + "\"").c_str());
#elif __linux__
		system(("xdg-open \"" + auth_request_url + "\"").c_str());
#endif
	}
	std::cout << "After granting permission, enter the token: ";
	std::string access_token;
	std::cin >> access_token;
	return access_token;
}

} // namespace duckdb
