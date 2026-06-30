#include <Geode/Geode.hpp>
#include <Geode/utils/web.hpp>
#include <Geode/utils/file.hpp>
#include <Geode/ui/Popup.hpp>
#include <Geode/binding/LevelEditorLayer.hpp>
#include <Geode/binding/EditorUI.hpp>
#include <Geode/binding/GameObject.hpp>
#include <Geode/binding/LevelSettingsObject.hpp>
#include <cocos2d.h>
#include <matjson.hpp>
#include <fmt/format.h>
#include <fstream>
#include <sstream>
#include <ctime>
#include <unordered_map>
#include <algorithm>

using namespace geode::prelude;
using namespace cocos2d;

// ═══════════════════════════════════════════════════════════════════
//  BASE64
// ═══════════════════════════════════════════════════════════════════

static const std::string B64 =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64Encode(const unsigned char* d, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned b = d[i] << 16;
        if (i+1 < len) b |= d[i+1] << 8;
        if (i+2 < len) b |= d[i+2];
        out += B64[(b>>18)&63]; out += B64[(b>>12)&63];
        out += (i+1 < len) ? B64[(b>>6)&63] : '=';
        out += (i+2 < len) ? B64[b&63]      : '=';
    }
    return out;
}

// ═══════════════════════════════════════════════════════════════════
//  SCREENSHOT
// ═══════════════════════════════════════════════════════════════════

struct Snap { std::string b64; bool ok = false; };

static Snap captureEditor() {
    auto* dir = CCDirector::sharedDirector();
    auto sz = dir->getWinSize();
    auto* rt = CCRenderTexture::create(
        (int)sz.width, (int)sz.height, kCCTexture2DPixelFormat_RGBA8888);
    if (!rt) return {};
    rt->begin();
    dir->getRunningScene()->visit();
    rt->end();
    auto* img = rt->newCCImage(false);
    if (!img) return {};
    auto path = Mod::get()->getSaveDir() / "gd_ai_snap.png";
    bool saved = img->saveToFile(path.string().c_str(), false);
    CC_SAFE_RELEASE(img);
    if (!saved) return {};
    auto raw = file::readBinary(path);
    if (!raw) return {};
    auto data = raw.unwrap();
    Snap s; s.b64 = b64Encode(data.data(), data.size()); s.ok = !s.b64.empty();
    return s;
}

// ═══════════════════════════════════════════════════════════════════
//  CONSTANTS
// ═══════════════════════════════════════════════════════════════════

static constexpr float GD_UNITS_PER_SEC = 311.f;  // Speed 1x
static constexpr int   CONFIDENCE_WARN  = 70;

// Object IDs safe for all players (always owned)
static const std::vector<int> SAFE_PALETTE = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10,
    211, 467, 1031, 1755, 1756,
    1329, 1330, 1331, 1332, 1334,
    899, 1006
};

// Style presets — detailed prompts for consistent results
static const std::vector<std::pair<std::string, std::string>> PRESETS = {
    {"HELL",   "hellfire demonic inferno - deep reds and burning oranges, pitch black bg, lava glow effects, bone and spike shapes, everything feels scorched and alive with fire"},
    {"SPACE",  "deep cosmic void - near-black bg, cyan and violet nebula glows, dense star particle fields, hovering crystal formations, dramatic lens flares and light streaks"},
    {"OCEAN",  "abyssal underwater depth - deep teal and midnight blue, bioluminescent glowing particles, soft coral and seaweed silhouettes, shimmering light rays filtering from above"},
    {"NEON",   "cyberpunk dystopia nightscape - absolute black bg, hot pink and electric blue neon line outlines, sharp geometric grid patterns, glitch pulse effects at intervals"},
    {"NATURE", "enchanted ancient forest - deep emerald greens and earthy browns, firefly glow particles, twisted vine and root silhouettes, warm dappled golden canopy light"},
};

// ═══════════════════════════════════════════════════════════════════
//  GEMINI URL
// ═══════════════════════════════════════════════════════════════════

static const std::string GEMINI_URL =
    "https://generativelanguage.googleapis.com/v1beta/models/"
    "gemini-2.0-flash:generateContent";

// ─── Build per-pass system prompt ────────────────────────────────

static std::string buildPassPrompt(int pass, bool ownedOnly,
                                   float secStart, float secEnd,
                                   float bpm) {
    float beatLen = GD_UNITS_PER_SEC * 60.f / bpm;

    std::string passStr;
    switch (pass) {
        case 0:
            passStr =
                "═══ PASS 1 — BACKGROUND LAYER ═══\n"
                "Place ONLY large background objects. z_layer MUST be -3.\n"
                "Scale range: 1.5–4.0. Object count: 15–25.\n"
                "These are massive atmospheric shapes far behind gameplay.\n"
                "Sparse placement — these set the mood, not the detail.";
            break;
        case 1:
            passStr =
                "═══ PASS 2 — MIDGROUND LAYER ═══\n"
                "Place ONLY midground detail objects. z_layer MUST be -1.\n"
                "Scale range: 0.8–2.0. Object count: 20–30.\n"
                "Fill space between background and gameplay with layered interest.\n"
                "Denser than pass 1. Avoid covering pass 1 objects entirely.";
            break;
        case 2:
            passStr = std::string(
                "═══ PASS 3 — FOREGROUND + TRIGGERS ═══\n"
                "Place small foreground accent objects AND all color/pulse triggers.\n"
                "z_layer for deco: 1 or 3. Scale: 0.3–1.2. Count: 10–20 deco objects.\n"
                "ALSO generate 4–8 color triggers and 3–5 pulse triggers.\n"
                "BPM = ") + std::to_string((int)bpm) + ". GD speed = ~311 units/sec.\n" +
                "Beat interval in units = " + std::to_string((int)beatLen) + ".\n" +
                "Snap trigger X positions to nearest beat grid (" +
                std::to_string((int)beatLen) + " unit intervals).";
            break;
        default:
            passStr = "Place decoration objects.";
    }

    std::string secStr = "";
    if (secStart >= 0.f && secEnd > secStart) {
        secStr = "\n\nSECTION CONSTRAINT: ONLY place objects between x=" +
            std::to_string((int)secStart) + " and x=" +
            std::to_string((int)secEnd) +
            ". Do NOT place anything outside this x range.\n";
    }

    std::string palStr = "";
    if (ownedOnly) {
        palStr = "\n\nOBJECT PALETTE (restricted): Only use IDs: "
            "1,2,3,4,5,6,7,8,9,10,211,467,1031,1755,1756,1329,1330,1331,1332,1334\n";
    }

    return R"(
You are an elite Geometry Dash level decorator matching the visual artistry of
Tidal Wave, Yatagarasu, Bloodbath, and Cataclysm. You understand depth layering,
color theory, and atmosphere-first decoration philosophy.

You are given a SCREENSHOT of the current GD editor state. Before generating,
carefully analyze:
  - Where the ground line is (the base most blocks are on)
  - Where the gameplay path runs (blocks, spikes, platforms)
  - What empty space is available (above, below, between objects)
  - What was already placed in previous passes (if any)

Your output MUST NOT overlap with gameplay objects.
Keep all decorations at least 30 units away from any gameplay block you see.

)" + passStr + secStr + palStr + R"(

Respond ONLY with raw JSON — absolutely no markdown, no backticks, no explanation text:

{
  "analysis": "one concise sentence describing what you see in the editor",
  "confidence": 85,
  "bg_color": {"r": 5, "g": 0, "b": 15},
  "ground_color": {"r": 10, "g": 0, "b": 30},
  "objects": [
    {
      "id": 211,
      "x": 150, "y": 300,
      "scale": 2.5, "rotation": 0,
      "flip_x": false,
      "z_layer": -3,
      "color_channel": 1
    }
  ],
  "color_channels": [
    {"channel": 1, "r": 255, "g": 50, "b": 200, "opacity": 1.0, "blending": true}
  ],
  "triggers": [
    {"type": "color", "x": 311, "target_channel": 1, "r": 200, "g": 0, "b": 80, "duration": 0.5, "blending": true},
    {"type": "pulse", "x": 622, "target_channel": 2, "r": 150, "g": 50, "b": 255, "fade_in": 0.1, "hold": 0.2, "fade_out": 0.4}
  ]
}

KEY RULES:
  - confidence: your 0–100 certainty about how well you can see the layout
  - z_layer must match the current pass instruction above
  - Best deco IDs: 211(glow sq), 1755(circle), 1756(diamond), 1031(small glow),
    467(triangle), 1329-1334(particles), 1616-1620(lens flares), 1/10(blocks)
  - Use color_channel 1-8 for dynamic color via triggers
  - Vary scales dramatically for depth perception
  - ONLY output the raw JSON object, nothing else
)";
}

// ═══════════════════════════════════════════════════════════════════
//  CHAT ENTRY
// ═══════════════════════════════════════════════════════════════════

struct ChatEntry {
    std::string sender;
    std::string message;
    std::string timestamp;
};

// ═══════════════════════════════════════════════════════════════════
//  AI DECO POPUP
// ═══════════════════════════════════════════════════════════════════

class AIDecoPopup : public FLAlertLayer {

    // ── UI nodes ─────────────────────────────────────────────────
    CCTextInputNode* m_promptInput   = nullptr;
    CCTextInputNode* m_bpmInput      = nullptr;
    CCTextInputNode* m_secStartInput = nullptr;
    CCTextInputNode* m_secEndInput   = nullptr;
    CCLabelBMFont*   m_statusLabel   = nullptr;
    CCLayer*         m_chatLayer     = nullptr;
    CCMenu*          m_actionMenu    = nullptr;

    // ── State ────────────────────────────────────────────────────
    float m_chatY         = 0.f;
    bool  m_busy          = false;
    bool  m_ownedOnly     = false;
    bool  m_previewMode   = true;
    float m_bpm           = 120.f;
    float m_secStart      = -1.f;
    float m_secEnd        = -1.f;
    std::string m_currentPrompt;
    std::string m_currentApiKey;

    // ── Object tracking ──────────────────────────────────────────
    std::vector<GameObject*>              m_previewObjects;
    std::vector<GameObject*>              m_lastPlaced;
    std::vector<std::vector<GameObject*>> m_passObjects;  // [pass][objs]

    // ── Chat history ─────────────────────────────────────────────
    std::vector<ChatEntry> m_chatHistory;

    // ── Network ──────────────────────────────────────────────────
    EventListener<Task<web::WebResponse, web::WebProgress>> m_listener;

    // ── Dimensions ───────────────────────────────────────────────
    static constexpr float PW     = 460.f;
    static constexpr float PH     = 420.f;
    static constexpr float CHAT_H = 190.f;

protected:
    bool init() {
        if (!FLAlertLayer::init(nullptr, "AI Deco Assistant", "Close", nullptr, PW))
            return false;
        m_mainLayer->removeAllChildren();
        buildUI();
        return true;
    }

    // ═════════════════════════════════════════════════════════════
    //  UI BUILD
    // ═════════════════════════════════════════════════════════════

    void buildUI() {
        // Re-add popup background (FLAlertLayer defaults were cleared)
        auto* popupBg = CCScale9Sprite::create("GJ_square01.png");
        popupBg->setContentSize({PW, PH});
        popupBg->setPosition({PW/2.f, PH/2.f});
        m_mainLayer->addChild(popupBg, -1);

        // Title
        auto* title = CCLabelBMFont::create("AI Deco Assistant", "goldFont.fnt");
        title->setScale(0.65f);
        title->setPosition({PW/2.f, PH - 18.f});
        m_mainLayer->addChild(title);

        auto* sub = CCLabelBMFont::create(
            "Gemini 2.0 Flash  |  3-Pass Vision  |  by D.M", "chatFont.fnt");
        sub->setScale(0.30f);
        sub->setColor({130, 100, 255});
        sub->setPosition({PW/2.f, PH - 34.f});
        m_mainLayer->addChild(sub);

        // Chat BG
        auto* chatBG = CCScale9Sprite::create("square02_001.png");
        chatBG->setContentSize({PW - 20.f, CHAT_H});
        chatBG->setPosition({PW/2.f, CHAT_H/2.f + 130.f});
        chatBG->setOpacity(55);
        chatBG->setColor({6, 0, 22});
        m_mainLayer->addChild(chatBG);

        // Clipping node for chat scroll
        auto* clip = CCClippingNode::create();
        clip->setContentSize({PW - 22.f, CHAT_H - 4.f});
        clip->setPosition({11.f, 132.f});
        clip->setAlphaThreshold(0.05f);
        auto* stencil = CCLayerColor::create({255,255,255,255});
        stencil->setContentSize({PW - 22.f, CHAT_H - 4.f});
        clip->setStencil(stencil);
        m_mainLayer->addChild(clip, 5);

        m_chatLayer = CCLayer::create();
        m_chatLayer->setPosition({8.f, CHAT_H - 8.f});
        clip->addChild(m_chatLayer);

        // ── Preset buttons ──────────────────────────────────────
        auto* presetMenu = CCMenu::create();
        presetMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(presetMenu, 10);

        float gap = (PW - 32.f) / (float)PRESETS.size();
        for (int i = 0; i < (int)PRESETS.size(); i++) {
            auto* spr = ButtonSprite::create(
                PRESETS[i].first.c_str(), "bigFont.fnt",
                "GJ_button_04.png", 0.4f);
            spr->setScale(0.65f);
            auto* btn = CCMenuItemSpriteExtra::create(
                spr, this, menu_selector(AIDecoPopup::onPreset));
            btn->setTag(i);
            btn->setPosition({16.f + i * gap + gap/2.f, 122.f});
            presetMenu->addChild(btn);
        }

        // ── BPM + Section row ────────────────────────────────────
        // BPM label + input
        addSmallLabel("BPM:", 22.f, 104.f);
        addInputBG(70.f, 104.f, 50.f);
        m_bpmInput = makeInput(50.f, 22.f, "120", 70.f, 104.f);

        // Section label + inputs
        addSmallLabel("X:", 128.f, 104.f);
        addInputBG(162.f, 104.f, 58.f);
        m_secStartInput = makeInput(54.f, 22.f, "start", 162.f, 104.f);

        addSmallLabel("to", 200.f, 104.f);
        addInputBG(232.f, 104.f, 58.f);
        m_secEndInput = makeInput(54.f, 22.f, "end", 232.f, 104.f);

        // Use selection button
        auto* selMenu = CCMenu::create();
        selMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(selMenu, 10);

        auto* selSpr = ButtonSprite::create("SEL", "bigFont.fnt", "GJ_button_05.png", 0.5f);
        selSpr->setScale(0.60f);
        auto* selBtn = CCMenuItemSpriteExtra::create(
            selSpr, this, menu_selector(AIDecoPopup::onUseSelection));
        selBtn->setPosition({285.f, 104.f});
        selMenu->addChild(selBtn);

        // ── Prompt input ─────────────────────────────────────────
        auto* inputBG = CCScale9Sprite::create("square02_001.png");
        inputBG->setContentSize({PW - 120.f, 36.f});
        inputBG->setPosition({(PW-120.f)/2.f + 5.f, 76.f});
        inputBG->setOpacity(130);
        inputBG->setColor({18, 8, 45});
        m_mainLayer->addChild(inputBG, 5);

        m_promptInput = CCTextInputNode::create(
            PW - 130.f, 30.f, "Describe your deco vibe...", "chatFont.fnt");
        m_promptInput->setPosition({(PW-120.f)/2.f + 5.f, 76.f});
        m_mainLayer->addChild(m_promptInput, 6);

        // ── Action buttons ───────────────────────────────────────
        m_actionMenu = CCMenu::create();
        m_actionMenu->setPosition({0.f, 0.f});
        m_mainLayer->addChild(m_actionMenu, 10);
        buildActionButtons(false);

        // Status bar
        m_statusLabel = CCLabelBMFont::create(
            "Type a vibe, pick a preset, and hit GO!", "chatFont.fnt");
        m_statusLabel->setScale(0.32f);
        m_statusLabel->setColor({160,160,255});
        m_statusLabel->setPosition({PW/2.f - 30.f, 14.f});
        m_mainLayer->addChild(m_statusLabel, 5);

        // Initial messages
        pushChat("AI: Ready! I will screenshot your layout before each pass.", {120,220,255});
        pushChat("AI: Pick a style preset or describe your own vibe below.", {120,220,255});
        pushChat("AI: Set BPM for beat-aligned triggers. Use SEL for section.", {100,200,220});
    }

    // ── Small UI helpers ──────────────────────────────────────────

    void addSmallLabel(const char* txt, float x, float y) {
        auto* lbl = CCLabelBMFont::create(txt, "chatFont.fnt");
        lbl->setScale(0.37f);
        lbl->setPosition({x, y});
        m_mainLayer->addChild(lbl, 5);
    }

    void addInputBG(float x, float y, float w) {
        auto* bg = CCScale9Sprite::create("square02_001.png");
        bg->setContentSize({w, 24.f});
        bg->setPosition({x, y});
        bg->setOpacity(120);
        bg->setColor({20, 10, 50});
        m_mainLayer->addChild(bg, 5);
    }

    CCTextInputNode* makeInput(float w, float h, const char* ph,
                               float x, float y) {
        auto* inp = CCTextInputNode::create(w, h, ph, "chatFont.fnt");
        inp->setPosition({x, y});
        m_mainLayer->addChild(inp, 6);
        return inp;
    }

    // ─── Action button layout (normal vs preview-pending) ─────────

    void buildActionButtons(bool previewPending) {
        m_actionMenu->removeAllChildren();

        if (previewPending) {
            // CONFIRM
            auto* cSpr = ButtonSprite::create("CONFIRM", "bigFont.fnt", "GJ_button_01.png", 0.6f);
            auto* cBtn = CCMenuItemSpriteExtra::create(
                cSpr, this, menu_selector(AIDecoPopup::onConfirmPreview));
            cBtn->setPosition({PW - 70.f, 50.f});
            m_actionMenu->addChild(cBtn);

            // REJECT
            auto* rSpr = ButtonSprite::create("REJECT", "bigFont.fnt", "GJ_button_06.png", 0.6f);
            auto* rBtn = CCMenuItemSpriteExtra::create(
                rSpr, this, menu_selector(AIDecoPopup::onRejectPreview));
            rBtn->setPosition({PW - 70.f, 26.f});
            m_actionMenu->addChild(rBtn);

        } else {
            // GO
            auto* goSpr = ButtonSprite::create("GO", "goldFont.fnt", "GJ_button_01.png", 1.f);
            auto* goBtn = CCMenuItemSpriteExtra::create(
                goSpr, this, menu_selector(AIDecoPopup::onSend));
            goBtn->setPosition({PW - 42.f, 76.f});
            m_actionMenu->addChild(goBtn);

            // UNDO
            auto* uSpr = ButtonSprite::create("UNDO", "bigFont.fnt", "GJ_button_06.png", 0.45f);
            uSpr->setScale(0.65f);
            auto* uBtn = CCMenuItemSpriteExtra::create(
                uSpr, this, menu_selector(AIDecoPopup::onUndo));
            uBtn->setPosition({PW - 42.f, 50.f});
            m_actionMenu->addChild(uBtn);

            // LOG (export)
            auto* lSpr = ButtonSprite::create("LOG", "bigFont.fnt", "GJ_button_05.png", 0.4f);
            lSpr->setScale(0.60f);
            auto* lBtn = CCMenuItemSpriteExtra::create(
                lSpr, this, menu_selector(AIDecoPopup::onExportChat));
            lBtn->setPosition({PW - 42.f, 28.f});
            m_actionMenu->addChild(lBtn);

            // OWN toggle
            auto* oSpr = ButtonSprite::create(
                m_ownedOnly ? "OWN:ON" : "OWN:OFF", "bigFont.fnt",
                m_ownedOnly ? "GJ_button_01.png" : "GJ_button_04.png", 0.38f);
            oSpr->setScale(0.60f);
            auto* oBtn = CCMenuItemSpriteExtra::create(
                oSpr, this, menu_selector(AIDecoPopup::onToggleOwned));
            oBtn->setPosition({330.f, 28.f});
            m_actionMenu->addChild(oBtn);

            // PREVIEW toggle
            auto* pSpr = ButtonSprite::create(
                m_previewMode ? "PRV:ON" : "PRV:OFF", "bigFont.fnt",
                m_previewMode ? "GJ_button_01.png" : "GJ_button_04.png", 0.38f);
            pSpr->setScale(0.60f);
            auto* pBtn = CCMenuItemSpriteExtra::create(
                pSpr, this, menu_selector(AIDecoPopup::onTogglePreview));
            pBtn->setPosition({385.f, 28.f});
            m_actionMenu->addChild(pBtn);
        }
    }

    // ═════════════════════════════════════════════════════════════
    //  CHAT HELPERS
    // ═════════════════════════════════════════════════════════════

    void pushChat(const std::string& msg, ccColor3B col = {230,230,230},
                  const std::string& sender = "AI") {
        
