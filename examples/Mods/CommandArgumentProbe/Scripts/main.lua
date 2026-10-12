-- Disabled by default. Observes numeric VM arguments and preserves every value.
-- Enable briefly to validate hook delivery; this does not invoke gameplay commands.
local arguments, commands = 0, 0
fdk.hook('script.command', 'pre', function()
    commands = commands + 1
end)
fdk.hook('script.command.argument', 'pre', function(group, command, script, fn, index, value)
    assert(math.type(index) == 'integer' and index >= 0 and index < 32)
    assert(type(value) == 'number' and value == value and math.abs(value) < math.huge)
    arguments = arguments + 1
    if arguments == 1 then
        print('First numeric argument:', group, command, script, fn, index, value)
    end
    return nil -- Preserve the exact original tagged DWORD.
end)
fdk.every(1, function()
    print('Observed commands and numeric arguments:', commands, arguments)
end)
fdk.on('stop', function()
    print('Final commands and numeric arguments:', commands, arguments)
end)
