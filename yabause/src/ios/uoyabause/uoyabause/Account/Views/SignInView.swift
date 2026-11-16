import SwiftUI
import AuthenticationServices
import CryptoKit
import FirebaseAuth
import FirebaseCore
import GoogleSignIn

/// Runs the Google and Apple sign-in flows and reports failures.
///
/// The Apple path generates a nonce, which the legacy LoginViewController did
/// not: it passed rawNonce: nil, so a stolen Apple identity token could be
/// replayed into this app. Apple only ever sees the SHA256 hash; Firebase
/// receives the raw value and checks that the token's nonce claim matches.
@MainActor
final class SignInViewModel: ObservableObject {
    @Published private(set) var isWorking = false
    @Published private(set) var errorMessage: String?

    /// Raw nonce of the in-flight Apple request.
    private var appleRawNonce: String?

    func clearError() {
        errorMessage = nil
    }

    // MARK: - Google

    func signInWithGoogle() {
        guard let clientID = FirebaseApp.app()?.options.clientID,
              let presenter = GameLauncher.topViewController() else {
            reportFailure()
            return
        }
        GIDSignIn.sharedInstance.configuration = GIDConfiguration(clientID: clientID)
        isWorking = true
        GIDSignIn.sharedInstance.signIn(withPresenting: presenter) { [weak self] result, error in
            MainActor.assumeIsolated {
                guard let self = self else { return }
                self.isWorking = false
                if let error = error {
                    // A user-cancelled sign-in is not a failure worth reporting.
                    if (error as NSError).code != GIDSignInError.canceled.rawValue {
                        self.reportFailure(error)
                    }
                    return
                }
                guard let user = result?.user,
                      let idToken = user.idToken?.tokenString else {
                    self.reportFailure()
                    return
                }
                let credential = GoogleAuthProvider.credential(
                    withIDToken: idToken, accessToken: user.accessToken.tokenString)
                self.signIn(with: credential)
            }
        }
    }

    // MARK: - Apple

    /// Stores the raw nonce and hands Apple only its hash.
    func prepareAppleRequest(_ request: ASAuthorizationAppleIDRequest) {
        let nonce = Self.randomNonceString()
        appleRawNonce = nonce
        request.requestedScopes = [.fullName, .email]
        request.nonce = Self.sha256(nonce)
    }

    func completeAppleSignIn(_ result: Result<ASAuthorization, Error>) {
        switch result {
        case .failure(let error):
            appleRawNonce = nil
            if (error as? ASAuthorizationError)?.code != .canceled {
                reportFailure(error)
            }
        case .success(let authorization):
            guard let credential = authorization.credential
                    as? ASAuthorizationAppleIDCredential,
                  let tokenData = credential.identityToken,
                  let idToken = String(data: tokenData, encoding: .utf8),
                  let rawNonce = appleRawNonce else {
                appleRawNonce = nil
                reportFailure()
                return
            }
            appleRawNonce = nil
            // Firebase 12 made the generic credential(withProviderID:idToken:rawNonce:)
            // overload Objective-C only. appleCredential() is the Swift entry point and
            // also forwards fullName, which Apple returns only on the first sign-in.
            let firebaseCredential = OAuthProvider.appleCredential(
                withIDToken: idToken, rawNonce: rawNonce, fullName: credential.fullName)
            signIn(with: firebaseCredential)
        }
    }

    // MARK: - Firebase

    private func signIn(with credential: AuthCredential) {
        isWorking = true
        Auth.auth().signIn(with: credential) { [weak self] _, error in
            MainActor.assumeIsolated {
                self?.isWorking = false
                if let error = error {
                    self?.reportFailure(error)
                }
                // On success AccountState's auth listener swaps the view.
            }
        }
    }

    private func reportFailure(_ error: Error? = nil) {
        errorMessage = String(localized: "Could not sign in.",
                              comment: "Sign in error")
        if let error = error {
            print("SignInViewModel: sign in failed: \(error)")
        }
    }

    // MARK: - Nonce

    /// Cryptographically random string used once per Apple request.
    nonisolated static func randomNonceString(length: Int = 32) -> String {
        let charset = Array("0123456789ABCDEFGHIJKLMNOPQRSTUVXYZabcdefghijklmnopqrstuvwxyz-._")
        var result = ""
        var remaining = length
        while remaining > 0 {
            var random: UInt8 = 0
            let status = SecRandomCopyBytes(kSecRandomDefault, 1, &random)
            guard status == errSecSuccess else {
                // SecRandomCopyBytes only fails when the system RNG is broken.
                fatalError("Unable to generate a nonce: OSStatus \(status)")
            }
            // Reject values that would bias the modulo below.
            if random < 248 {
                result.append(charset[Int(random) % charset.count])
                remaining -= 1
            }
        }
        return result
    }

    nonisolated static func sha256(_ input: String) -> String {
        SHA256.hash(data: Data(input.utf8))
            .map { String(format: "%02x", $0) }
            .joined()
    }
}

/// The signed-out account tab: what signing in buys, and the two providers.
struct SignInView: View {
    @StateObject private var viewModel = SignInViewModel()
    @Environment(\.colorScheme) private var colorScheme

    var body: some View {
        VStack(spacing: 24) {
            Spacer(minLength: 0)

            Image(systemName: "person.crop.circle")
                .font(.system(size: 72))
                .foregroundStyle(.secondary)

            Text(String(localized: "Sign In", comment: "Sign in heading"))
                .font(.title2.bold())

            VStack(alignment: .leading, spacing: 12) {
                benefit(icon: "externaldrive.badge.icloud",
                        text: String(localized: "Back up your saves to the cloud.",
                                     comment: "Sign in benefit"))
                benefit(icon: "trophy",
                        text: String(localized: "Earn RetroAchievements.",
                                     comment: "Sign in benefit"))
                benefit(icon: "list.number",
                        text: String(localized: "Compete on the leaderboards.",
                                     comment: "Sign in benefit"))
            }
            .padding(.horizontal, 8)

            VStack(spacing: 12) {
                Button {
                    viewModel.signInWithGoogle()
                } label: {
                    Text(String(localized: "Sign in with Google",
                                comment: "Google sign in button"))
                        .frame(maxWidth: .infinity, minHeight: 44)
                }
                .buttonStyle(.borderedProminent)
                .foregroundStyle(Color.themeOnPrimary)

                SignInWithAppleButton(.signIn) { request in
                    viewModel.prepareAppleRequest(request)
                } onCompletion: { result in
                    viewModel.completeAppleSignIn(result)
                }
                .frame(height: 44)
                .signInWithAppleButtonStyle(colorScheme == .dark ? .white : .black)
            }
            .disabled(viewModel.isWorking)

            if viewModel.isWorking {
                ProgressView()
            }

            Spacer(minLength: 0)
        }
        .padding(32)
        .alert(String(localized: "Error", comment: "Error alert title"),
               isPresented: Binding(get: { viewModel.errorMessage != nil },
                                    set: { if !$0 { viewModel.clearError() } })) {
            Button("OK", role: .cancel) {}
        } message: {
            Text(viewModel.errorMessage ?? "")
        }
    }

    private func benefit(icon: String, text: String) -> some View {
        HStack(spacing: 12) {
            Image(systemName: icon)
                .frame(width: 24)
                .foregroundStyle(Color.themePrimary)
            Text(text)
                .font(.subheadline)
            Spacer(minLength: 0)
        }
    }
}
