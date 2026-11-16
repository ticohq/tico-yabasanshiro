package org.uoyabause.android

import android.content.ClipData
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.util.Log
import android.view.DragEvent
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AppCompatActivity
import androidx.credentials.CredentialManager
import androidx.credentials.CustomCredential
import androidx.credentials.GetCredentialRequest
import androidx.documentfile.provider.DocumentFile
import androidx.lifecycle.lifecycleScope
import com.firebase.ui.auth.AuthUI
import com.firebase.ui.auth.FirebaseAuthUIActivityResultContract
import com.google.android.gms.auth.api.signin.GoogleSignIn
import com.google.android.gms.auth.api.signin.GoogleSignInOptions
import com.google.android.gms.common.api.ApiException
import com.google.android.libraries.identity.googleid.GetGoogleIdOption
import com.google.android.libraries.identity.googleid.GetSignInWithGoogleOption
import com.google.android.libraries.identity.googleid.GoogleIdTokenCredential
import com.google.firebase.auth.FirebaseAuth
import com.google.firebase.auth.GoogleAuthProvider
import com.google.firebase.firestore.FirebaseFirestore
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import org.devmiyax.yabasanshiro.R

/**
 * Debug-only probe for Google Play Games on PC.
 *
 * Launch with:
 *   adb shell am start -n org.devmiyax.yabasanshioro2.debug/org.uoyabause.android.PcProbeActivity
 * Every result is logged under the "PcProbe" tag and shown on screen.
 */
class PcProbeActivity : AppCompatActivity() {
    private lateinit var logView: TextView

    private val authUiLauncher =
        registerForActivityResult(FirebaseAuthUIActivityResultContract()) { result ->
            val err = result.idpResponse?.error
            val cause = err?.cause
            val apiCode = (cause as? ApiException)?.statusCode
            log(
                "AuthUI result code=${result.resultCode} " +
                    "errorCode=${err?.errorCode} msg=${err?.message} " +
                    "cause=${cause?.javaClass?.name}:${cause?.message} apiStatus=$apiCode " +
                    "currentUser=${FirebaseAuth.getInstance().currentUser?.uid}",
            )
        }

    private val openDocLauncher =
        registerForActivityResult(ActivityResultContracts.OpenMultipleDocuments()) { uris ->
            log("OpenDocument returned ${uris.size} uri(s)")
            uris.forEach { probeUri(it) }
        }

    private val openTreeLauncher =
        registerForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri ->
            if (uri == null) {
                log("OpenDocumentTree cancelled")
                return@registerForActivityResult
            }
            log("OpenDocumentTree uri=$uri authority=${uri.authority}")
            try {
                val children = DocumentFile.fromTreeUri(this, uri)?.listFiles().orEmpty()
                log("  children=${children.size}: ${children.take(10).joinToString { it.name ?: "?" }}")
            } catch (e: Exception) {
                log("  list failed: ${e.javaClass.name}: ${e.message}")
            }
        }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }

        fun button(
            label: String,
            action: () -> Unit,
        ) = root.addView(
            Button(this).apply {
                text = label
                setOnClickListener { action() }
            },
        )

        button("1. AuthUI Google (current path)") { startAuthUi() }
        button("2. CredMan SignInWithGoogle button flow") { credManSignIn(useButtonFlow = true) }
        button("3. CredMan GetGoogleIdOption (all accounts)") { credManSignIn(useButtonFlow = false) }
        button("4. Legacy GoogleSignIn.silentSignIn") { legacySilent() }
        button("5. SAF open documents (multiple)") { openDocLauncher.launch(arrayOf("*/*")) }
        button("6. SAF open tree") { openTreeLauncher.launch(null) }
        button("7. Firestore read") { firestoreRead() }
        button("8. Sign out") { signOut() }
        logView = TextView(this).apply { text = "Drop files anywhere on this screen.\n" }
        root.addView(logView)

        val scroll = ScrollView(this).apply { addView(root) }
        scroll.setOnDragListener { _, event -> onDrag(event) }
        setContentView(scroll)

        val pc = packageManager.hasSystemFeature("com.google.android.play.feature.HPE_EXPERIENCE")
        log("device=${Build.MANUFACTURER}/${Build.MODEL} sdk=${Build.VERSION.SDK_INT} isPc=$pc")
        log("currentUser=${FirebaseAuth.getInstance().currentUser?.uid}")
    }

    private fun log(msg: String) {
        Log.i(TAG, msg)
        runOnUiThread { logView.append(msg + "\n") }
    }

    private fun startAuthUi() {
        val providers =
            arrayListOf(
                AuthUI.IdpConfig.EmailBuilder().build(),
                AuthUI.IdpConfig.GoogleBuilder().build(),
            )
        authUiLauncher.launch(
            AuthUI
                .getInstance()
                .createSignInIntentBuilder()
                .setAvailableProviders(providers)
                .build(),
        )
    }

    private fun credManSignIn(useButtonFlow: Boolean) {
        val clientId = getString(R.string.default_web_client_id)
        val option =
            if (useButtonFlow) {
                GetSignInWithGoogleOption.Builder(clientId).build()
            } else {
                GetGoogleIdOption
                    .Builder()
                    .setServerClientId(clientId)
                    .setFilterByAuthorizedAccounts(false)
                    .build()
            }
        val request = GetCredentialRequest.Builder().addCredentialOption(option).build()
        lifecycleScope.launch {
            try {
                val response = CredentialManager.create(this@PcProbeActivity).getCredential(this@PcProbeActivity, request)
                val cred = response.credential
                log("CredMan got credential type=${cred.type} class=${cred.javaClass.name}")
                if (cred is CustomCredential && cred.type == GoogleIdTokenCredential.TYPE_GOOGLE_ID_TOKEN_CREDENTIAL) {
                    val google = GoogleIdTokenCredential.createFrom(cred.data)
                    log("  google id=${google.id} tokenLen=${google.idToken.length}")
                    val authResult =
                        FirebaseAuth
                            .getInstance()
                            .signInWithCredential(GoogleAuthProvider.getCredential(google.idToken, null))
                            .await()
                    log("  Firebase signIn uid=${authResult.user?.uid} providers=${authResult.user?.providerData?.map { it.providerId }}")
                }
            } catch (e: Exception) {
                log("CredMan failed: ${e.javaClass.name}: ${e.message}")
            }
        }
    }

    private fun legacySilent() {
        val gso =
            GoogleSignInOptions
                .Builder(GoogleSignInOptions.DEFAULT_SIGN_IN)
                .requestIdToken(getString(R.string.default_web_client_id))
                .requestEmail()
                .build()
        GoogleSignIn
            .getClient(this, gso)
            .silentSignIn()
            .addOnSuccessListener { log("silentSignIn ok tokenLen=${it.idToken?.length}") }
            .addOnFailureListener { log("silentSignIn failed: ${(it as? ApiException)?.statusCode} ${it.message}") }
    }

    private fun firestoreRead() {
        val uid = FirebaseAuth.getInstance().currentUser?.uid ?: "anonymous-probe"
        lifecycleScope.launch {
            try {
                val snap =
                    FirebaseFirestore
                        .getInstance()
                        .collection("users")
                        .document(uid)
                        .get()
                        .await()
                log("Firestore read ok exists=${snap.exists()} fromCache=${snap.metadata.isFromCache}")
            } catch (e: Exception) {
                log("Firestore read failed: ${e.javaClass.name}: ${e.message}")
            }
        }
    }

    private fun signOut() {
        AuthUI.getInstance().signOut(this).addOnCompleteListener { log("signed out") }
    }

    private fun onDrag(event: DragEvent): Boolean {
        when (event.action) {
            DragEvent.ACTION_DRAG_STARTED -> {
                val desc = event.clipDescription
                val mimes = (0 until (desc?.mimeTypeCount ?: 0)).map { desc!!.getMimeType(it) }
                log("DRAG_STARTED mimes=$mimes label=${desc?.label}")
            }
            DragEvent.ACTION_DROP -> {
                val perms = if (Build.VERSION.SDK_INT >= 24) requestDragAndDropPermissions(event) else null
                val clip: ClipData? = event.clipData
                log("DROP items=${clip?.itemCount} perms=${perms != null}")
                for (i in 0 until (clip?.itemCount ?: 0)) {
                    val item = clip!!.getItemAt(i)
                    log("  item[$i] uri=${item.uri} text=${item.text} intent=${item.intent}")
                    item.uri?.let { probeUri(it) }
                }
            }
            DragEvent.ACTION_DRAG_ENDED -> log("DRAG_ENDED result=${event.result}")
        }
        return true
    }

    private fun probeUri(uri: Uri) {
        lifecycleScope.launch {
            val msg =
                withContext(Dispatchers.IO) {
                    try {
                        val doc = DocumentFile.fromSingleUri(this@PcProbeActivity, uri)
                        val head = ByteArray(16)
                        val n = contentResolver.openInputStream(uri)?.use { it.read(head) } ?: -1
                        val fd = contentResolver.openFileDescriptor(uri, "r")?.use { it.statSize }
                        "  uri=$uri authority=${uri.authority} name=${doc?.name} size=${doc?.length()} " +
                            "fdStatSize=$fd head=${head.take(maxOf(n, 0)).joinToString(" ") { "%02x".format(it) }}"
                    } catch (e: Exception) {
                        "  open $uri failed: ${e.javaClass.name}: ${e.message}"
                    }
                }
            log(msg)
        }
    }

    companion object {
        private const val TAG = "PcProbe"
    }
}
