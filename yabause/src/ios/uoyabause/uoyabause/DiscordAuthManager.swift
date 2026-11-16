import UIKit
import Foundation
import FirebaseAuth
import FirebaseFirestore
import FirebaseCore
import CryptoKit
import CommonCrypto

/**
 * Discord OAuth2 authentication provider with PKCE support for Firebase Auth integration
 */
class DiscordAuthManager {
    private let TAG = "DiscordAuthManager"
    private var auth: Auth {
        // Firebaseが初期化されているか確認
        if FirebaseApp.app() == nil {
            // GoogleService-Info.plistからFirebaseを初期化
            var filePath: String?

            #if DEBUG
            filePath = Bundle.main.path(forResource: "GoogleService-Info-Debug", ofType: "plist")
            #else
            filePath = Bundle.main.path(forResource: "GoogleService-Info", ofType: "plist")
            #endif

            if let filePath = filePath, let options = FirebaseOptions(contentsOfFile: filePath) {
                FirebaseApp.configure(options: options)
            } else {
                print("\(TAG): Couldn't find correct GoogleService-Info.plist file.")
            }
        }
        return Auth.auth()
    }

    private var firestore: Firestore {
        return Firestore.firestore()
    }

    // PKCE constants
    private let CODE_VERIFIER_LENGTH = 64 // Recommended length (43-128 chars)
    private let COLLECTION_DISCORD_LINKS = "discord_links"

    // Configuration from secrets.plist
    private var CLIENT_ID: String = ""
    private var REDIRECT_URI: String = ""
    private var DISCORD_AUTH_URL: String = ""
    private var DISCORD_TOKEN_URL: String = ""
    private var DISCORD_USER_URL: String = ""

    init() {
        loadConfiguration()
    }

    private func loadConfiguration() {
        if let path = Bundle.main.path(forResource: "secrets", ofType: "plist"),
           let dict = NSDictionary(contentsOfFile: path) as? [String: Any] {

            CLIENT_ID = dict["discord_client_id"] as? String ?? ""
            REDIRECT_URI = dict["discord_redirect_uri"] as? String ?? ""
            DISCORD_AUTH_URL = dict["discord_auth_url"] as? String ?? ""
            DISCORD_TOKEN_URL = dict["discord_token_url"] as? String ?? ""
            DISCORD_USER_URL = dict["discord_user_url"] as? String ?? ""
        }
    }

    /**
     * Start the Discord OAuth2 authorization flow with PKCE
     */
    func startDiscordLogin() {
        // Generate code verifier and challenge
        let tokenStorage = TokenStorage()
        let localCodeVerifier = generateCodeVerifier()
        let codeChallenge = generateCodeChallenge(localCodeVerifier)

        // Store the code verifier securely with a 10-minute timeout
        tokenStorage.saveCodeVerifier(localCodeVerifier)

        print("\(TAG): Generated PKCE Code Verifier: \(localCodeVerifier)")
        print("\(TAG): Generated PKCE Code Challenge: \(codeChallenge)")

        // Build the authorization URL with PKCE parameters
        guard var urlComponents = URLComponents(string: DISCORD_AUTH_URL) else {
            print("\(TAG): Invalid Discord Auth URL")
            return
        }

        urlComponents.queryItems = [
            URLQueryItem(name: "client_id", value: CLIENT_ID),
            URLQueryItem(name: "redirect_uri", value: REDIRECT_URI),
            URLQueryItem(name: "response_type", value: "code"),
            URLQueryItem(name: "scope", value: "identify"),
            URLQueryItem(name: "code_challenge", value: codeChallenge),
            URLQueryItem(name: "code_challenge_method", value: "S256")
        ]

        guard let authUrl = urlComponents.url else {
            print("\(TAG): Failed to build Discord Auth URL")
            return
        }

        // Open the URL in Safari
        DispatchQueue.main.async {
            UIApplication.shared.open(authUrl)
            print(": Opening Discord Auth URL: \(authUrl)")
        }
    }

    /**
     * Handle the redirect URI from Discord OAuth2 authorization
     * @param url The redirect URI with authorization code
     * @return Boolean true if the process succeeded, false otherwise
     */
    func handleRedirectAndSignIn(url: URL) async -> Bool {
        // Check for errors in the redirect
        let components = URLComponents(url: url, resolvingAgainstBaseURL: false)

        if let error = components?.queryItems?.first(where: { $0.name == "error" })?.value {
            let errorDescription = components?.queryItems?.first(where: { $0.name == "error_description" })?.value ?? error
            print("\(TAG): OAuth Error: \(error) - \(errorDescription)")
            TokenStorage().clearCodeVerifier()
            return false
        }

        // Extract the authorization code
        guard let code = components?.queryItems?.first(where: { $0.name == "code" })?.value else {
            print("\(TAG): Authorization code not found in redirect URI")
            TokenStorage().clearCodeVerifier()
            return false
        }

        print("\(TAG): Received authorization code: \(code)")

        // Retrieve the stored code verifier from secure storage
        let tokenStorage = TokenStorage()
        guard let storedCodeVerifier = tokenStorage.getCodeVerifier() else {
            print("\(TAG): Code verifier not found - authentication session may have timed out")
            return false
        }

        // Clear the code verifier immediately to prevent reuse
        tokenStorage.clearCodeVerifier()

        do {
            // 1. Exchange the code for access token
            guard let tokenResponse = try await exchangeCodeForTokenResponse(code: code, codeVerifier: storedCodeVerifier) else {
                return false
            }

            guard let accessToken = tokenResponse["access_token"] as? String else {
                print("\(TAG): Access token not found in response")
                return false
            }

            let refreshToken = tokenResponse["refresh_token"] as? String ?? ""
            let expiresIn = tokenResponse["expires_in"] as? TimeInterval ?? 0

            print("\(TAG): Access token obtained successfully")

            // Store tokens securely for future use
            tokenStorage.saveDiscordTokens(accessToken: accessToken, refreshToken: refreshToken, expiresIn: expiresIn)

            // 2. Fetch user info from Discord
            guard let userInfo = try await getUserInfo(accessToken: accessToken) else {
                return false
            }

            guard let discordId = userInfo["id"] as? String else {
                print("\(TAG): Discord ID not found in user info")
                return false
            }

            print("\(TAG): Discord user info obtained for ID: \(discordId)")

            // 3. Update Firebase user profile with Discord info
            let firebaseSuccess = await updateFirebaseWithDiscordInfo(discordId: discordId, accessToken: accessToken, userInfo: userInfo)
            if !firebaseSuccess {
                print("\(TAG): Failed to update Firebase with Discord info")
                return false
            }

            if let username = userInfo["username"] as? String {
                print("\(TAG): Discord OAuth flow completed successfully for user: \(username)")
            }

            return true
        } catch {
            print("\(TAG): Error during Discord authentication process: \(error.localizedDescription)")
            return false
        }
    }

    /**
     * Exchange authorization code for access token using PKCE
     * @param code The authorization code from Discord
     * @param codeVerifier The original code verifier used in the authorization request
     * @return Dictionary The token response
     * @throws Error on network error or failed response
     */
    private func exchangeCodeForTokenResponse(code: String, codeVerifier: String) async throws -> [String: Any]? {
        var request = URLRequest(url: URL(string: DISCORD_TOKEN_URL)!)
        request.httpMethod = "POST"
        request.addValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        request.addValue("application/json", forHTTPHeaderField: "Accept")

        let parameters = [
            "client_id": CLIENT_ID,
            "grant_type": "authorization_code",
            "code": code,
            "redirect_uri": REDIRECT_URI,
            "code_verifier": codeVerifier
        ]

        let bodyString = parameters.map { "\($0.key)=\($0.value)" }.joined(separator: "&")
        request.httpBody = bodyString.data(using: .utf8)

        print("\(TAG): Requesting token from Discord")

        let (data, response) = try await URLSession.shared.data(for: request)

        guard let httpResponse = response as? HTTPURLResponse else {
            print("\(TAG): Invalid response type")
            return nil
        }

        guard httpResponse.statusCode == 200 else {
            let errorBody = String(data: data, encoding: .utf8) ?? "Empty body"
            print("\(TAG): Token exchange failed: \(httpResponse.statusCode) - \(errorBody)")
            throw NSError(domain: "DiscordAuthError", code: httpResponse.statusCode, userInfo: [NSLocalizedDescriptionKey: "Token exchange failed: \(httpResponse.statusCode)"])
        }

        guard let jsonObject = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            print("\(TAG): Invalid JSON response")
            return nil
        }

        return jsonObject
    }

    /**
     * Get user information from Discord API
     * @param accessToken The Discord access token
     * @return Dictionary The user info
     * @throws Error on network error or failed response
     */
    private func getUserInfo(accessToken: String) async throws -> [String: Any]? {
        var request = URLRequest(url: URL(string: DISCORD_USER_URL)!)
        request.httpMethod = "GET"
        request.addValue("Bearer \(accessToken)", forHTTPHeaderField: "Authorization")
        request.addValue("application/json", forHTTPHeaderField: "Accept")

        let (data, response) = try await URLSession.shared.data(for: request)

        guard let httpResponse = response as? HTTPURLResponse else {
            print("\(TAG): Invalid response type")
            return nil
        }

        guard httpResponse.statusCode == 200 else {
            let errorBody = String(data: data, encoding: .utf8) ?? "Empty body"
            print("\(TAG): User info request failed: \(httpResponse.statusCode) - \(errorBody)")
            throw NSError(domain: "DiscordAuthError", code: httpResponse.statusCode, userInfo: [NSLocalizedDescriptionKey: "User info request failed: \(httpResponse.statusCode)"])
        }

        guard let jsonObject = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            print("\(TAG): Invalid JSON response")
            return nil
        }

        return jsonObject
    }

    /**
     * Update Firebase user profile with Discord info
     * @param discordId The Discord user ID
     * @param accessToken The Discord access token
     * @param userInfo The Discord user info
     * @return Boolean true if the update was successful, false otherwise
     */
    private func updateFirebaseWithDiscordInfo(discordId: String, accessToken: String, userInfo: [String: Any]) async -> Bool {
        guard let currentUser = auth.currentUser else {
            print("\(TAG): No Firebase user is currently signed in")
            return false
        }

        let discordDisplayName = userInfo["username"] as? String ?? "Discord User"
        var discordAvatarUrl = ""

        if let avatarHash = userInfo["avatar"] as? String {
            discordAvatarUrl = "https://cdn.discordapp.com/avatars/\(discordId)/\(avatarHash).png"
        }

        do {
            // 1. Store Discord link in Firestore
            try await storeDiscordLink(userId: currentUser.uid, discordId: discordId)

            // 2. Update Firebase user profile with Discord info
            // We'll store the Discord display name and avatar, but preserve the Firebase UID
            try await updateUserProfile(user: currentUser, displayName: discordDisplayName, photoURL: discordAvatarUrl)

            // 3. Store additional Discord info in Firestore to ensure we have both IDs
            try await storeDiscordUserInfo(userId: currentUser.uid, discordId: discordId, displayName: discordDisplayName, avatarUrl: discordAvatarUrl)

            return true
        } catch {
            print("\(TAG): Error updating Firebase with Discord info: \(error.localizedDescription)")
            return false
        }
    }

    /**
     * Store Discord link in Firestore
     * @param userId The Firebase user ID
     * @param discordId The Discord user ID
     */
    private func storeDiscordLink(userId: String, discordId: String) async throws {
        let data: [String: Any] = [
            "discord_id": discordId,
            "linked_at": Date().timeIntervalSince1970 * 1000 // Timestamp in milliseconds
        ]

        try await firestore.collection(COLLECTION_DISCORD_LINKS).document(userId).setData(data)
    }

    /**
     * Update Firebase user profile
     * @param user The Firebase user
     * @param displayName The display name
     * @param photoURL The photo URL
     */
    private func updateUserProfile(user: User, displayName: String, photoURL: String) async throws {
        let changeRequest = user.createProfileChangeRequest()

        // Only update if the user doesn't already have a display name
        if user.displayName == nil || user.displayName?.isEmpty == true {
            changeRequest.displayName = displayName
        }

        // Only update if the user doesn't already have a photo URL
        if user.photoURL == nil && !photoURL.isEmpty {
            changeRequest.photoURL = URL(string: photoURL)
        }

        try await changeRequest.commitChanges()
    }

    /**
     * Store Discord user info in Firestore
     * @param userId The Firebase user ID
     * @param discordId The Discord user ID
     * @param displayName The Discord display name
     * @param avatarUrl The Discord avatar URL
     */
    private func storeDiscordUserInfo(userId: String, discordId: String, displayName: String, avatarUrl: String) async throws {
        let data: [String: Any] = [
            "discord_id": discordId,
            "display_name": displayName,
            "avatar_url": avatarUrl,
            "updated_at": Date().timeIntervalSince1970 * 1000 // Timestamp in milliseconds
        ]

        try await firestore.collection("users").document(userId).setData(data, merge: true)
    }

    // MARK: - PKCE Utilities

    /**
     * Generate a random code verifier for PKCE
     * @return String The code verifier
     */
    private func generateCodeVerifier() -> String {
        var buffer = [UInt8](repeating: 0, count: CODE_VERIFIER_LENGTH)
        _ = SecRandomCopyBytes(kSecRandomDefault, buffer.count, &buffer)
        return Data(buffer).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
            .trimmingCharacters(in: .whitespaces)
    }

    /**
     * Generate a code challenge from a code verifier using SHA256
     * @param codeVerifier The code verifier
     * @return String The code challenge
     */
    private func generateCodeChallenge(_ codeVerifier: String) -> String {
        guard let data = codeVerifier.data(using: .utf8) else { return "" }

        var digest = [UInt8](repeating: 0, count: Int(CC_SHA256_DIGEST_LENGTH))
        data.withUnsafeBytes {
            _ = CC_SHA256($0.baseAddress, CC_LONG(data.count), &digest)
        }

        return Data(digest).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
            .trimmingCharacters(in: .whitespaces)
    }
}

/**
 * TokenStorage for securely storing Discord tokens and PKCE state
 */
class TokenStorage {
    private let TAG = "DiscordTokenStorage"
    private let prefsName = "discord_auth_prefs"
    private let keyAccessToken = "discord_access_token"
    private let keyRefreshToken = "discord_refresh_token"
    private let keyExpiresAt = "discord_expires_at"
    private let keyCodeVerifier = "discord_code_verifier"
    private let keyCodeVerifierTimestamp = "discord_code_verifier_timestamp"
    private let userDefaults = UserDefaults.standard

    /**
     * Save Discord tokens to UserDefaults
     * @param accessToken The Discord access token
     * @param refreshToken The Discord refresh token
     * @param expiresIn The token expiration time in seconds
     */
    func saveDiscordTokens(accessToken: String, refreshToken: String, expiresIn: TimeInterval) {
        let expiresAtMillis = Date().timeIntervalSince1970 + expiresIn
        print("\(TAG): Saving Discord tokens, expires in \(expiresIn) seconds")

        userDefaults.set(accessToken, forKey: keyAccessToken)
        userDefaults.set(refreshToken, forKey: keyRefreshToken)
        userDefaults.set(expiresAtMillis, forKey: keyExpiresAt)
        userDefaults.synchronize()
    }

    /**
     * Get the Discord access token if it's still valid
     * @return String? The access token or nil if expired
     */
    func getAccessToken() -> String? {
        let expiresAt = userDefaults.double(forKey: keyExpiresAt)
        let currentTime = Date().timeIntervalSince1970

        if expiresAt > currentTime {
            return userDefaults.string(forKey: keyAccessToken)
        }

        return nil
    }

    /**
     * Save the PKCE code verifier
     * @param codeVerifier The code verifier to save
     * @param timeoutMinutes How long the code verifier should be valid (in minutes)
     */
    func saveCodeVerifier(_ codeVerifier: String, timeoutMinutes: Int = 10) {
        let expiresAtMillis = Date().timeIntervalSince1970 + Double(timeoutMinutes * 60)
        print("\(TAG): Saving PKCE code verifier, expires in \(timeoutMinutes) minutes")

        userDefaults.set(codeVerifier, forKey: keyCodeVerifier)
        userDefaults.set(expiresAtMillis, forKey: keyCodeVerifierTimestamp)
        userDefaults.synchronize()
    }

    /**
     * Get the stored code verifier if it's still valid
     * @return String? The code verifier or nil if expired
     */
    func getCodeVerifier() -> String? {
        let expiresAt = userDefaults.double(forKey: keyCodeVerifierTimestamp)
        let currentTime = Date().timeIntervalSince1970

        if expiresAt > currentTime {
            return userDefaults.string(forKey: keyCodeVerifier)
        }

        clearCodeVerifier()
        return nil
    }

    /**
     * Clear the stored code verifier
     */
    func clearCodeVerifier() {
        userDefaults.removeObject(forKey: keyCodeVerifier)
        userDefaults.removeObject(forKey: keyCodeVerifierTimestamp)
        userDefaults.synchronize()
        print("\(TAG): PKCE code verifier cleared")
    }

    /**
     * Clear all stored Discord tokens and PKCE state
     */
    func clearTokens() {
        userDefaults.removeObject(forKey: keyAccessToken)
        userDefaults.removeObject(forKey: keyRefreshToken)
        userDefaults.removeObject(forKey: keyExpiresAt)
        userDefaults.removeObject(forKey: keyCodeVerifier)
        userDefaults.removeObject(forKey: keyCodeVerifierTimestamp)
        userDefaults.synchronize()
        print("\(TAG): Discord tokens and PKCE state cleared")
    }
}
