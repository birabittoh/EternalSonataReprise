#include <algorithm>
#include <bit>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>

#include "generated/eternalsonata_init.h"

#include <rex/cvar.h>
#include <rex/system/kernel_state.h>

#include "eternalsonata_asset_container.h"
#include "eternalsonata_asset_system.h"
#include "eternalsonata_hooks_internal.h"
#include "native_renderer_frame.h"
#include "overworld_system.h"

// ---------------------------------------------------------------------------
// Console references in the shipped text
// ---------------------------------------------------------------------------
//
// The save, New Game and Unlock Key screens talk about storage devices, gamer
// profiles, signing in and switching off the Xbox 360, none of which exist in
// the port. Each affected string gets a rewrite per language.
//
// A string a mod changed keeps the mod's text.
//
// The blobs sit in read-only guest pages, so the strings cannot be edited in
// place; the fixed copies live on the guest heap and the BTX lookup hands them
// out instead (see the sub_8223B780 hook in eternalsonata_options.cpp).

namespace eternalsonata_hooks {
namespace {

// BTX blobs baked into the image that hold console text.
constexpr u32 kUnlockKeyText = 0x8202B8A8;
constexpr u32 kSystemText = 0x82031A00;
constexpr u32 kSaveText = 0x822F94F0;
constexpr u32 kNewGameText = 0x822FDD00;

// Written as UTF-8 with real line breaks for readability; converted to the
// blobs' Latin-1 (Shift-JIS for JPN) and "\n" markup when installed. English
// covers USA and GBR, and a null keeps the stock text.
struct ConsoleTextFix {
    u32 blob;
    u32 string_id;
    const char* en;
    const char* fr;
    const char* it;
    const char* de;
    const char* es;
    const char* ja;
};

constexpr ConsoleTextFix kConsoleTextFixes[] = {
    {kUnlockKeyText, 4,
     "The save data can no longer\nbe accessed.",
     "Les données de sauvegarde ne sont\nplus accessibles.",
     "I dati di salvataggio non sono\npiù accessibili.",
     "Auf die Speicherdaten kann nicht\nmehr zugegriffen werden.",
     "Ya no se puede acceder a los\ndatos guardados.",
     "セーブデータに\nアクセスできなくなりました。"},
    {kUnlockKeyText, 6,
     "Unable to check the Unlock Key.",
     "Impossible de vérifier la clé de déblocage.",
     "Impossibile verificare la chiave di attivazione.",
     "Freischalt-Code kann nicht überprüft werden.",
     "No se pudo comprobar la clave de desbloqueo.",
     "アンロックキーを確認できません。"},
    {kUnlockKeyText, 7,
     "Checking Unlock Key.\nPlease do not close the game.\n",
     "Vérification de clé de déblocage.\nNe pas fermer le jeu.\n",
     "Verifica chiave di attivazione.\nNon chiudere il gioco.\n",
     "Freischalt-Code wird überprüft.\nBitte das Spiel nicht beenden.\n",
     "Comprobando clave de desbloqueo.\nNo cierres el juego.\n",
     "アンロックキーのチェック中です…。\nゲームを終了しないでください。\n"},

    {kSystemText, 182,
     "An error has occurred.\nReturning to Title Screen",
     "Une erreur est survenue.\nRetour à l'écran de titre.",
     "Si è verificato un errore.\nTorna alla schermata del titolo.",
     "Ein Fehler ist aufgetreten.\nZurück zum Titelbildschirm.",
     "Se ha producido un error.\nRegresarás a la pantalla de inicio.",
     "エラーが発生しました。\nタイトル画面に戻ります。"},

    {kSaveText, 0,
     "Checking save files...\nPlease do not close the game.\n",
     "Vérification des fichiers de sauvegarde en cours.\nNe pas fermer le jeu.\n",
     "Verifica del salvataggio in corso...\nNon chiudere il gioco.",
     "Gespeicherte Spielstände werden geprüft ...\nBitte das Spiel nicht beenden.\n",
     "Comprobando los archivos de guardado...\nNo cierres el juego.\n",
     "セーブファイルをチェックしています…。\nゲームを終了しないでください。"},
    {kSaveText, 3,
     "Saving...\nPlease do not close the game.\n",
     "Sauvegarde en cours.\nNe pas fermer le jeu.\n",
     "Salvataggio in corso...\nNon chiudere il gioco.",
     "Speichern ...\nBitte das Spiel nicht beenden.\n",
     "Guardando...\nNo cierres el juego.\n",
     "データを書き込んでいます…。\nゲームを終了しないでください。\n"},
    {kSaveText, 7,
     "Loading...\nPlease do not close the game.\n",
     "Chargement en cours.\nNe pas fermer le jeu.\n",
     "Caricamento in corso...\nNon chiudere il gioco.",
     "Laden ...\nBitte das Spiel nicht beenden.\n",
     "Cargando...\nNo cierres el juego.\n",
     "データを読み込んでいます…。\nゲームを終了しないでください。"},
    {kSaveText, 12,
     "Insufficient disk space.",
     "Espace disque insuffisant.",
     "Spazio su disco insufficiente.",
     "Unzureichender Speicherplatz.",
     "No hay espacio suficiente en el disco.",
     "ディスクの空き容量が足りないため、セーブできません"},
    {kSaveText, 17,
     "There was an error checking the save file.\n",
     "Erreur lors de la vérification du fichier de sauvegarde.\n",
     "Si è verificato un errore durante il controllo del salvataggio.\n",
     "Beim Prüfen der Speicherdatei ist ein Fehler aufgetreten.\n",
     "Se produjo un error al comprobar el archivo de guardado.\n",
     "セーブファイルのチェック中にエラーが発生しました。\n"},
    {kSaveText, 18,
     "An unexpected error has occurred.\nThe save data cannot be accessed.\n",
     "Une erreur est survenue.\nAccès aux données de sauvegarde impossible.\n",
     "Si è verificato un errore inatteso.\nImpossibile accedere ai dati di salvataggio.\n",
     "Ein unerwarteter Fehler ist aufgetreten.\nAuf die Speicherdaten kann nicht zugegriffen werden.\n",
     "Se ha producido un error inesperado.\nNo se puede acceder a los datos guardados.\n",
     "予期しないエラーが発生したため、\nセーブデータにアクセスできません。\n"},
    {kSaveText, 22,
     "An unexpected error has occurred.\nThe save data cannot be accessed.\n",
     "Une erreur est survenue.\nAccès aux données de sauvegarde impossible.\n",
     "Si è verificato un errore inatteso.\nImpossibile accedere ai dati di salvataggio.\n",
     "Ein unerwarteter Fehler ist aufgetreten.\nAuf die Speicherdaten kann nicht zugegriffen werden.\n",
     "Se ha producido un error inesperado.\nNo se puede acceder a los datos guardados.\n",
     "予期しないエラーが発生したため、\nセーブデータにアクセスできません。\n"},
    {kSaveText, 24,
     "Saving is not available.\n",
     "La sauvegarde n'est pas disponible.\n",
     "Il salvataggio non è disponibile.",
     "Speichern ist nicht verfügbar.\n",
     "No es posible guardar.\n",
     "セーブできません。\n"},
    {kSaveText, 25,
     "The save data can no longer be accessed.\n",
     "Les données de sauvegarde ne sont plus accessibles.\n",
     "I dati di salvataggio non sono\npiù accessibili.",
     "Auf die Speicherdaten kann nicht\nmehr zugegriffen werden.\n",
     "Ya no se puede acceder a los\ndatos guardados.\n",
     "セーブデータにアクセスできなくなりました。\n"},
    {kSaveText, 26,
     "There is no Eternal Sonata save data.\n",
     "Aucune sauvegarde d'Eternal Sonata n'a été détectée.\n",
     "Nessun salvataggio di Eternal Sonata trovato.\n",
     "Es wurden keine Spieldaten von Eternal Sonata gefunden.\n",
     "No hay datos guardados de Eternal Sonata.\n",
     "トラスティベルのセーブファイルがありません。\n"},
    {kSaveText, 29,
     "Loading is not available.\n",
     "Le chargement n'est pas disponible.\n",
     "Il caricamento non è disponibile.",
     "Laden ist nicht verfügbar.\n",
     "No es posible cargar.\n",
     "ロードできません。\n"},
    {kSaveText, 37,
     "The save data may have become\ninaccessible during game play.\n",
     "Les données de sauvegarde sont peut-être devenues\ninaccessibles en cours de partie.\n",
     "I dati di salvataggio potrebbero essere diventati\ninaccessibili durante il gioco.\n",
     "Auf die Speicherdaten konnte während\ndes Spiels eventuell nicht zugegriffen werden.\n",
     "Es posible que no se haya podido acceder a los\ndatos guardados durante la partida.\n",
     "ゲームプレイ中にセーブデータに\nアクセスできなくなった可能性があります。\n"},
    {kSaveText, 40,
     "Insufficient disk space.\nGame data has not been saved.\n\nContinue without saving?",
     "Espace disque insuffisant.\n\nLes données de jeu n'ont pas été sauvegardées. \nContinuer sans sauvegarder ?",
     "Spazio su disco insufficiente. \nI dati di gioco non sono stati salvati.\nContinuare senza salvare?",
     "Unzureichender Speicherplatz. \nDer Spielstand wurde nicht gespeichert.\nOhne zu speichern fortfahren?",
     "No hay espacio suficiente en el disco.\n\nNo se han guardado los datos del juego. ¿Salir de todos modos?",
     "ディスクの空き容量が足りないため、\nデータを保存することは出来ません。\n\nセーブされていませんが、セーブを終了しますか？"},

    {kNewGameText, 1,
     "Preparing save data.\n",
     "Préparation des données de sauvegarde.\n",
     "Preparazione dei dati di salvataggio.",
     "Speicherdaten werden vorbereitet.\n",
     "Preparando los datos guardados.\n",
     "セーブデータを準備しています。\n"},
    {kNewGameText, 2,
     "Checking disk space...\nPlease do not close the game.\n",
     "Vérification de l'espace libre sur le disque...\nNe pas fermer le jeu.\n",
     "Controllo spazio su disco...\nNon chiudere il gioco.",
     "Speicherplatz wird geprüft...\nBitte das Spiel nicht beenden.\n",
     "Comprobando el espacio en disco...\nNo cierres el juego.\n",
     "空き容量をチェック中です……。\nゲームを終了しないでください。\n"},
    {kNewGameText, 3,
     nullptr,
     "Espace disque suffisant. \nLancement de la partie en cours.\n",
     nullptr,
     nullptr,
     "Hay suficiente espacio en el disco.\nIniciando el juego.",
     nullptr},
    {kNewGameText, 4,
     "Insufficient disk space.\nGame data will not be saved.\nStart game anyway?\n",
     "L'espace disque est insuffisant.\nLes données de jeu ne seront pas sauvegardées.\n\nLancer quand même la partie ?\n",
     "Spazio su disco insufficiente.\nI dati di gioco non saranno salvati.\n\nAvviare il gioco comunque?\n",
     "Es ist nicht genügend Speicherplatz\nverfügbar.\nSpieldaten werden nicht gespeichert.\n\nSpiel trotzdem starten?\n",
     "No hay espacio suficiente en el disco.\nNo se guardarán los datos del juego.\n\n¿Deseas iniciar el juego de todos modos?\n",
     "ディスクの空き容量が足りません。\nこの状態ではデータは保存されません。\n\nこのままゲームを開始しますか？"},
    {kNewGameText, 6,
     "No save location is available.\nGame data will not be saved.\nStart game anyway?\n",
     "Aucun emplacement de sauvegarde n'est\ndisponible. Les données de jeu\nne seront pas sauvegardées.\nLancer quand même la partie ?\n",
     "Nessuna posizione di salvataggio\ndisponibile. I dati di gioco\nnon saranno salvati.\nAvviare il gioco comunque?",
     "Es ist kein Speicherort verfügbar.\nSpieldaten werden nicht gespeichert.\nSpiel trotzdem starten?\n",
     "No hay ninguna ubicación de guardado\ndisponible. No se guardarán\nlos datos del juego.\n¿Deseas iniciar el juego de todos modos?\n",
     "データの保存先がありません。\nこの状態ではデータは保存されません。\n\nこのままゲームを開始しますか？"},
    {kNewGameText, 8,
     "Game data will not be saved.\nStart game anyway?\n",
     "Les données de jeu ne seront pas sauvegardées.\n\nLancer quand même la partie ?\n",
     "I dati di gioco non saranno salvati.\n\nAvviare il gioco comunque?",
     "Spieldaten werden nicht gespeichert.\nSpiel trotzdem starten?\n",
     "No se guardarán los datos del juego.\n\n¿Deseas iniciar el juego de todos modos?\n",
     "この状態ではデータは保存されません。\n\nこのままゲームを開始しますか？"},
    {kNewGameText, 9,
     "The save data could not be accessed.\nGame data will not be saved.\nStart game anyway?\n",
     "Impossible d'accéder aux données de sauvegarde.\nLes données de jeu ne seront pas sauvegardées.\n\nLancer quand même la partie ?\n",
     "Impossibile accedere ai dati di salvataggio. \nI dati di gioco non saranno salvati.\nAvviare il gioco comunque?",
     "Zugriff auf die Speicherdaten nicht möglich. \nSpieldaten werden nicht gespeichert.\nSpiel trotzdem starten?\n",
     "No se pudo acceder a los datos guardados. \nNo se guardarán los datos del juego.\n\n¿Deseas iniciar el juego de todos modos?\n",
     "セーブデータにアクセスできませんでした。\nこの状態ではデータは保存されません。\n\nこのままゲームを開始しますか？"},
};

// The rewrite for the language block tagged `lang`.
const char* TextFor(const ConsoleTextFix& fix, u32 lang) {
    switch (lang) {
    case 0x4A504E20:  // "JPN "
        return fix.ja;
    case 0x55534120:  // "USA "
    case 0x47425220:  // "GBR "
        return fix.en;
    case 0x46524120:  // "FRA "
        return fix.fr;
    case 0x49544120:  // "ITA "
        return fix.it;
    case 0x44455520:  // "DEU "
        return fix.de;
    case 0x45535020:  // "ESP "
        return fix.es;
    default:
        return nullptr;
    }
}

// UTF-8 to the blobs' Latin-1, with line breaks as their "\n" markup.
std::string ToBlobText(const char* utf8) {
    std::string out;
    for (const char* p = utf8; *p;) {
        const u8 c = static_cast<u8>(*p);
        if (c == '\n') {
            out += "\\n";
            ++p;
        } else if ((c & 0xE0) == 0xC0 && p[1]) {
            out.push_back(static_cast<char>(((c & 0x1F) << 6) | (p[1] & 0x3F)));
            p += 2;
        } else {
            out.push_back(static_cast<char>(c));
            ++p;
        }
    }
    return out;
}

u32 CopyToGuest(u8* base, const std::string& text) {
    auto* mem = rex::system::kernel_memory();
    const u32 copy = mem ? mem->SystemHeapAlloc(text.size() + 1, 0x20) : 0;
    if (!copy) {
        REXLOG_WARN("[text] guest allocation failed for \"{}\"", text);
        return 0;
    }
    for (size_t i = 0; i <= text.size(); ++i) {
        REX_STORE_U8(copy + i, static_cast<u8>(text[i]));
    }
    return copy;
}

std::once_flag g_console_text_once;
// Stock string address -> our copy. The lookup returns addresses straight out
// of the language blocks, and the blobs are static, so this is built once.
std::unordered_map<u32, u32> g_console_text;

// Walks each blob's language blocks the way sub_8223B780 does (layout in
// scripts/btx.py) to find where every string to fix lives.
void BuildConsoleText(u8* base) {
    for (const ConsoleTextFix& fix : kConsoleTextFixes) {
        const u32 languages = REX_LOAD_U32(fix.blob + 0x0C);
        u32 block = fix.blob + REX_LOAD_U32(fix.blob + 0x04);
        for (u32 l = 0; l < languages; ++l) {
            const u32 lang = REX_LOAD_U32(block);
            const char fourcc[5] = {char(lang >> 24), char(lang >> 16), char(lang >> 8),
                                    char(lang), 0};
            const char* const text = eternalsonata::XexTextModded(fix.blob, fourcc, fix.string_id)
                                         ? nullptr
                                         : TextFor(fix, lang);
            const u32 table = block + REX_LOAD_U32(block + 0x04);
            const u32 count = REX_LOAD_U32(block + 0x10);
            for (u32 i = 0; text && i < count; ++i) {
                if (REX_LOAD_U32(table + 8 * i) != fix.string_id) {
                    continue;
                }
                const u32 stock = block + REX_LOAD_U32(table + 8 * i + 4);
                std::string encoded;
                if (lang == 0x4A504E20) {
                    eternalsonata::assets::EncodeShiftJis(text, encoded);
                } else {
                    encoded = ToBlobText(text);
                }
                if (const u32 copy = CopyToGuest(base, encoded)) {
                    g_console_text[stock] = copy;
                }
                break;
            }
            block += REX_LOAD_U32(block + 0x08);
        }
    }
    REXLOG_INFO("[text] {} console strings rewritten", g_console_text.size());
}

}  // namespace

u32 ConsoleTextOverrideFor(u8* base, u32 text_address) {
    std::call_once(g_console_text_once, BuildConsoleText, base);
    const auto it = g_console_text.find(text_address);
    return it != g_console_text.end() ? it->second : 0;
}

}  // namespace eternalsonata_hooks

// ---------------------------------------------------------------------------
// Debug hooks
// ---------------------------------------------------------------------------

// sub_82254060 is the devkit-privilege gate called early in xstart (the title
// entry point).  It probes XexCheckExecutablePrivilege(0xA), XGetAVPack, and
// two ExGetXConfigSetting calls; if any check indicates a non-dev retail
// environment it returns 1, which makes xstart call XamLoaderTerminateTitle
// and kill the process.  In the recompiled port we are always "dev-capable"
// so the simplest fix is to override the whole function and return 0 (pass).
REX_EXTERN(__imp__sub_82254060);
REX_HOOK_RAW(sub_82254060) { ctx.r3.u64 = 0; }

// sub_82108180(camera, vertical_fov, near, far) is the only projection setup in
// the binary. It derives the projection matrix at camera+304, the stored field
// of view at camera+368 and the near plane extents at camera+380..392 from that
// one angle, so scaling the argument moves the rendered view and the frustum
// the cull test below uses together.
REXCVAR_DECLARE(double, camera_fov_scale);
REX_EXTERN(__imp__sub_82108180);
REX_HOOK_RAW(sub_82108180) {
    const double scale = std::clamp(REXCVAR_GET(camera_fov_scale), 0.5, 2.0);
    const double fov = ctx.f1.f64;
    // Radians, and the cameras seen so far sit at pi/2. Anything outside a
    // plausible angle is not the value this hook thinks it is.
    if (scale == 1.0 || fov <= 0.01 || fov >= 2.5) {
        __imp__sub_82108180(ctx, base);
        return;
    }

    const u32 camera = ctx.r3.u32;
    ctx.f1.f64 = fov * scale;
    __imp__sub_82108180(ctx, base);

    // sub_821078B0 re-runs this every frame with the angle stored back at
    // camera+368, so leaving the scaled one there multiplies it again each
    // frame. Put the guest's own angle back and let only the projection and
    // the extents derived from it carry the scale.
    if (camera) {
        REX_STORE_U32(camera + 368, std::bit_cast<u32>(static_cast<float>(fov)));
    }
}

// The renderer widens the world after the guest has already culled its models.
// Give the guest sphere test the same field of view for this call. A window
// wider than 16:9 expands horizontally and a taller one vertically, so both
// extent pairs have to follow their own axis.
REX_EXTERN(__imp__sub_82108878);
REX_HOOK_RAW(sub_82108878) {
    const u32 camera = ctx.r3.u32;
    float clip_x = 1.0f;
    float clip_y = 1.0f;
    eternalsonata::FrameWorldClipScale(&clip_x, &clip_y);

    // Top, bottom, right and left extents at the near plane. Bottom and left
    // are negative, so a plain divide widens them the right way.
    struct Extent {
        u32 offset;
        float scale;
    };
    const Extent extents[] = {
        {380, clip_y}, {384, clip_y}, {388, clip_x}, {392, clip_x},
    };

    u32 saved[std::size(extents)] = {};
    bool widened = false;
    if (camera) {
        for (size_t i = 0; i < std::size(extents); ++i) {
            saved[i] = REX_LOAD_U32(camera + extents[i].offset);
            if (extents[i].scale >= 1.0f || extents[i].scale <= 0.0f) {
                continue;
            }
            REX_STORE_U32(camera + extents[i].offset,
                          std::bit_cast<u32>(std::bit_cast<float>(saved[i]) /
                                             extents[i].scale));
            widened = true;
        }
    }

    __imp__sub_82108878(ctx, base);

    if (widened) {
        for (size_t i = 0; i < std::size(extents); ++i) {
            REX_STORE_U32(camera + extents[i].offset, saved[i]);
        }
    }
}

// The debug-console (sub_822DFA88) and ConsoleSetting (sub_822E5BE8) init
// hooks used to live here.  Both forced "console active" state bytes after
// the original init ran, and both were removed as dead code: the retail build
// keeps only the allocation and teardown of those objects.  Their state bytes
// (dword_8244DDE0, byte_8244DDE8, ...) have zero reads after init, the command
// buffer at 0x8244C188 is only ever zeroed, and the console vtable
// off_82082DDC holds just a destructor and a nullsub - no render, input, or
// command-dispatch method survives in the binary.  An on-screen console has to
// be built host-side.

// ---------------------------------------------------------------------------
// Skippable voice waits
// ---------------------------------------------------------------------------

// A message ending in `<wv>` (control code 13, emitted by the markup
// preprocessor sub_821D50A8) waits for its voice clip and offers no way to cut
// it short. The consumer is the code-13 case of the layout pass sub_821D5CC0
// (jump table word_82082230, case at 0x821D69AC): on first arrival it parks the
// record in state 8 (a2+4) while sub_821431C0(dword_8243D89C, handle) says the
// clip at mgr+31372 is still playing, and advances once that returns 0.
//
// The advance button is already decoded for the `<w>` case: sub_821D49A0
// latches it into byte_8255D125 (mgr+31381) right before calling the layout
// pass. So when the record is parked on a voice and that flag is set, stop the
// clip the way the game's own skip path in sub_821D96F8 does
// (sub_82142EE8(mgr, handle, fade 0)) and answer "not playing" for that handle
// during this pass. A line the player does not touch still ends on its own.
//
// Only the captured handle is answered, so a `<vN>` later in the same pass
// starts a new clip that is waited on normally.
namespace {
constexpr u32 kTextManager = 0x82555690;
constexpr u32 kVoiceHandleOffset = 31372;
constexpr u32 kAdvancePressedOffset = 31381;
constexpr u32 kSoundManagerPtr = 0x8243D89C;
constexpr u32 kStateWaitingForVoice = 8;
u32 g_skipped_voice_handle = 0;
}  // namespace

REX_EXTERN(__imp__sub_821D5CC0);
REX_EXTERN(__imp__sub_82142EE8);
REX_HOOK_RAW(sub_821D5CC0) {
    const u32 mgr = ctx.r3.u32;
    const u32 record = ctx.r4.u32;
    g_skipped_voice_handle = 0;
    if (mgr == kTextManager && record && REX_LOAD_U32(record + 4) == kStateWaitingForVoice &&
        REX_LOAD_U8(mgr + kAdvancePressedOffset)) {
        const u32 handle = REX_LOAD_U32(mgr + kVoiceHandleOffset);
        if (handle) {
            ctx.r3.u32 = REX_LOAD_U32(kSoundManagerPtr);
            ctx.r4.u32 = handle;
            ctx.f1.f64 = 0.0;
            __imp__sub_82142EE8(ctx, base);
            ctx.r3.u32 = mgr;
            ctx.r4.u32 = record;
            g_skipped_voice_handle = handle;
        }
    }
    __imp__sub_821D5CC0(ctx, base);
    g_skipped_voice_handle = 0;
}

REX_EXTERN(__imp__sub_821431C0);
REX_HOOK_RAW(sub_821431C0) {
    if (g_skipped_voice_handle && ctx.r4.u32 == g_skipped_voice_handle) {
        ctx.r3.u32 = 0;
        return;
    }
    __imp__sub_821431C0(ctx, base);
}

// ---------------------------------------------------------------------------
// Text window content
// ---------------------------------------------------------------------------

// sub_821D3890 is SetText(mgr, window_id, text, tag) on the global text manager
// (dword_82555690).  It copies the raw markup into the window record at +8, or
// into the 1024 byte overflow buffer at 0x8255DFC2 for strings of 400 bytes or
// more.  Hooked rather than the preprocessor because it fires exactly once per
// "this window now shows this string".
REX_EXTERN(__imp__sub_821D3890);
REX_HOOK_RAW(sub_821D3890) {
    const u32 window = ctx.r4.u32;
    const u32 text = ctx.r5.u32;
    if (text) {
        eternalsonata::NotifyOverworldDialogue(
            window, reinterpret_cast<const char*>(base + text));
    }
    __imp__sub_821D3890(ctx, base);
}

// ---------------------------------------------------------------------------
// Storage-device textboxes
// ---------------------------------------------------------------------------
//
// sub_8223FB78 drives the storage-device screen: a1[99] is the next state,
// a1[100] selects state 7's message. Two states only narrate Xbox 360 storage
// selection and are skipped straight to their successor:
//
//   state 6                 "Please select a storage device."   -> 4, opens
//                                                                  the selector
//   state 7 with a1[100]==4 "There is enough available space."  -> 1, commits
//                                                                  the device
//
// Other a1[100] values are real errors ("not signed in", "insufficient
// space") and are left alone.
//
// Rewriting the state in the driver rather than in sub_8223FC88's entry action
// is required: the driver assigns a1[98] = a1[99] right after the action, so a
// change made there is swallowed and the successor never starts.

static constexpr u32 kStorageStateNext = 99 * 4;
static constexpr u32 kStorageMessageId = 100 * 4;

REX_EXTERN(__imp__sub_8223FB78);
REX_HOOK_RAW(sub_8223FB78) {
    const u32 a1 = ctx.r3.u32;
    if (a1) {
        const u32 state = REX_LOAD_U32(a1 + kStorageStateNext);
        if (state == 6) {
            REX_STORE_U32(a1 + kStorageStateNext, 4);
        } else if (state == 7 && REX_LOAD_U32(a1 + kStorageMessageId) == 4) {
            REX_STORE_U32(a1 + kStorageStateNext, 1);
        }
    }

    __imp__sub_8223FB78(ctx, base);
}
