-- Keep the supported Sky 1 session updating while another window has focus.
-- Sky 2's existing game preference is left to its own adapter.
fdk.on("start", function()
    local available, game = pcall(fdk.call, "game.name")
    if not available or game ~= "Trails in the Sky 1st Chapter" then return end
    fdk.engine.framework.run_in_background = true
    fdk.log("Sky 1 background updates enabled")
end)
