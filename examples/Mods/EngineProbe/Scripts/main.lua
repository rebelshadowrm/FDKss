-- Enable this mod by adding EngineProbe/enabled.txt to either supported Sky deployment.
-- Native inspection calls return JSON strings with fresh snapshots, not retained objects.
local elapsed = 0
fdk.on("start", function()
    print("Engine probe", fdk.call("game.name"), fdk.call("game.update_info"))
    print("Framework", fdk.call("game.framework"))
    print("Objects", fdk.call("game.objects"))
    print("Scene", fdk.call("game.scene"))
    print("Player", fdk.call("game.player"))
end)
fdk.on("update", function(dt)
    elapsed = elapsed + dt
    if elapsed >= 2 then
        elapsed = 0
        print("Update", fdk.call("game.update_info"))
        print("Framework", fdk.call("game.framework"))
        print("Objects", fdk.call("game.objects"))
        print("Scene", fdk.call("game.scene"))
        print("Player", fdk.call("game.player"))
    end
end)
fdk.on("stop", function()
    print("Engine probe stopped")
end)
