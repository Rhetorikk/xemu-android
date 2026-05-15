package app.xemu

import android.content.Intent
import android.content.SharedPreferences
import android.net.Uri
import android.os.Bundle
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.result.contract.ActivityResultContracts
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import org.json.JSONObject
import java.io.File

/**
 * First-run UX: pick MCPX BIOS, flash ROM, optional EEPROM/HDD/DVD via SAF,
 * persist the resulting filesDir paths, then launch EmulatorActivity with
 * the resolved config JSON.
 */
class LauncherActivity : AppCompatActivity() {

    private lateinit var prefs: SharedPreferences

    private val picker = registerForActivityResult(
        ActivityResultContracts.OpenDocument()
    ) { uri: Uri? -> uri?.let { handlePicked(it) } }

    private var pendingKind: String? = null

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        prefs = getSharedPreferences("xemu", MODE_PRIVATE)

        setContentView(buildUi())
    }

    private fun buildUi(): android.view.View {
        // Programmatic UI to keep the foundation small; later passes can swap
        // in a Compose/material screen.
        val ctx = this
        val root = android.widget.LinearLayout(ctx).apply {
            orientation = android.widget.LinearLayout.VERTICAL
            setPadding(48, 96, 48, 48)
        }

        fun heading(text: String) = android.widget.TextView(ctx).apply {
            this.text = text
            textSize = 22f
            setPadding(0, 0, 0, 16)
        }
        fun line(text: String) = android.widget.TextView(ctx).apply {
            this.text = text
            textSize = 14f
            setPadding(0, 4, 0, 16)
        }
        fun pickBtn(label: String, kind: String) =
            android.widget.Button(ctx).apply {
                this.text = label
                setOnClickListener { startPick(kind) }
            }

        root.addView(heading(getString(R.string.launcher_title)))
        root.addView(line(getString(R.string.launcher_subtitle)))

        root.addView(pickBtn(getString(R.string.pick_bios),   "bios"))
        root.addView(statusFor("bios"))
        root.addView(pickBtn(getString(R.string.pick_flash),  "flash"))
        root.addView(statusFor("flash"))
        root.addView(pickBtn(getString(R.string.pick_eeprom), "eeprom"))
        root.addView(statusFor("eeprom"))
        root.addView(pickBtn(getString(R.string.pick_hdd),    "hdd"))
        root.addView(statusFor("hdd"))
        root.addView(pickBtn(getString(R.string.pick_dvd),    "dvd"))
        root.addView(statusFor("dvd"))

        val start = android.widget.Button(ctx).apply {
            text = getString(R.string.start_emulation)
            setOnClickListener { tryStart() }
        }
        root.addView(start)

        return android.widget.ScrollView(ctx).apply { addView(root) }
    }

    private fun statusFor(kind: String): android.widget.TextView {
        val path = prefs.getString("path_$kind", null)
        return android.widget.TextView(this).apply {
            text = if (path != null)
                getString(R.string.status_picked, kind, File(path).name)
            else
                getString(R.string.status_missing, kind)
            textSize = 12f
            setPadding(0, 0, 0, 24)
        }
    }

    private fun startPick(kind: String) {
        pendingKind = kind
        picker.launch(arrayOf("*/*"))
    }

    private fun handlePicked(uri: Uri) {
        val kind = pendingKind ?: return
        pendingKind = null

        val size = SafCopier.sizeOf(this, uri)
        if (size > SafCopier.MAX_INLINE_COPY_BYTES &&
            kind != "bios" && kind != "flash" && kind != "eeprom") {
            AlertDialog.Builder(this)
                .setTitle(kind)
                .setMessage(getString(R.string.warn_large_image))
                .setPositiveButton(android.R.string.ok, null)
                .show()
            return
        }

        Toast.makeText(this, getString(R.string.status_copying, kind), Toast.LENGTH_SHORT).show()
        val out = SafCopier.copy(this, uri, "$kind.bin") ?: return
        prefs.edit().putString("path_$kind", out.absolutePath).apply()
        recreate()
    }

    private fun tryStart() {
        val bios = prefs.getString("path_bios", null)
        val flash = prefs.getString("path_flash", null)
        if (bios == null || flash == null) {
            Toast.makeText(this, getString(R.string.not_ready), Toast.LENGTH_LONG).show()
            return
        }
        val cfg = JSONObject().apply {
            put("bios",     bios)
            put("flash",    flash)
            put("eeprom",   prefs.getString("path_eeprom", "") ?: "")
            put("hdd",      prefs.getString("path_hdd", "")    ?: "")
            put("dvd",      prefs.getString("path_dvd", "")    ?: "")
            put("data_dir", filesDir.absolutePath)
        }.toString()

        startActivity(Intent(this, EmulatorActivity::class.java).apply {
            putExtra("config_json", cfg)
        })
    }
}
