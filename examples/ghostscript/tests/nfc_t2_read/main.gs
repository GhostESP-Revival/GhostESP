-- Read-only smoke test for the bounded Type-2 NDEF facade.
ghost.ui.set_title("NFC Type-2 test")

if not ghost.nfc.is_available() then
    print("local NFC reader unavailable")
    return
end

if not ghost.nfc.scan_start() then
    print("scan could not start (reader may be in use)")
    return
end

for _ = 1, 20 do
    if not ghost.nfc.scan_active() then break end
    ghost.delay(250)
end

local ok, tag_or_error = pcall(ghost.nfc.read, 256)
ghost.nfc.stop()

if not ok then
    print("read failed: " .. tostring(tag_or_error))
    return
end

if tag_or_error then
    print("model: " .. tag_or_error.model .. " uid: " .. tag_or_error.uid)
    print("user bytes: " .. tag_or_error.user_bytes .. " ndef bytes: " .. tag_or_error.ndef_length)
    print("read-only: " .. tostring(tag_or_error.read_only) .. " password: " .. tostring(tag_or_error.password_protected))
else
    print("no Type-2 NDEF tag found")
end
