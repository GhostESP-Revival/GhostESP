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

local tag = ghost.nfc.read(256)
if tag then
    print("model: " .. tag.model .. " uid: " .. tag.uid)
    print("user bytes: " .. tag.user_bytes .. " ndef bytes: " .. tag.ndef_length)
    print("read-only: " .. tostring(tag.read_only) .. " password: " .. tostring(tag.password_protected))
else
    print("no Type-2 NDEF tag found")
end

ghost.nfc.stop()
