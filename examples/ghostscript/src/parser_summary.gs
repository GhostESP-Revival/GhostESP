-- Show parser summaries for app-relative files.
-- Requires matching nfc, ir, or subghz permission.

local checks = {
    { "SubGHz", ghost.parser.subghz_summary, "captures/example.sub" },
    { "IR", ghost.parser.ir_summary, "infrared/example.ir" },
    { "NFC", ghost.parser.nfc_summary, "nfc/example.nfc" },
}

for _, item in ipairs(checks) do
    local label, fn, path = item[1], item[2], item[3]
    local summary = fn(path)
    if summary then
        print(label .. " summary for " .. path .. ":")
        print(summary)
    else
        print(label .. " summary unavailable for " .. path)
    end
end
