function align() return 8 end

function arguments(data)
    local src, index, lea = data.src, data.index, data.lea
    if index == 0 and lea == 0 then writeln("movq " .. src .. ", %rdi") return 0 end
    if index == 1 and lea == 0 then writeln("movq " .. src .. ", %rsi") return 0 end
    if index == 2 and lea == 0 then writeln("movq " .. src .. ", %rdx") return 0 end
    if index == 3 and lea == 0 then writeln("movq " .. src .. ", %rcx") return 0 end
    if index == 4 and lea == 0 then writeln("movq " .. src .. ", %r8d") return 0 end
    if index == 5 and lea == 0 then writeln("movq " .. src .. ", %r9d") return 0 end
    if index >= 6 and lea == 0 then writeln("pushq " .. src) return 0 end

    if index == 0 and lea ~= 0 then writeln("leaq " .. src .. ", %rdi") return 0 end
    if index == 1 and lea ~= 0 then writeln("leaq " .. src .. ", %rsi") return 0 end
    if index == 2 and lea ~= 0 then writeln("leaq " .. src .. ", %rdx") return 0 end
    if index == 3 and lea ~= 0 then writeln("leaq " .. src .. ", %rcx") return 0 end
    if index == 4 and lea ~= 0 then writeln("leaq " .. src .. ", %r8d") return 0 end
    if index == 5 and lea ~= 0 then writeln("leaq " .. src .. ", %r9d") return 0 end
    if index >= 6 and lea ~= 0 then writeln("pushq " .. src) return 0 end
end


function rax(data)
    local size = data.size
    if size == 1 then return "%al"  end
    if size == 2 then return "%ax"  end
    if size == 4 then return "%eax" end
    if size == 8 then return "%rax" end
end

function mov(data)
    local size = data.size
    if size == 1 then return "movb" end
    if size == 2 then return "movw" end
    if size == 4 then return "movl" end
    if size == 8 then return "movq" end
end

function extern(data)
    tabs(0)
    writeln(".extern " .. data.identifier)
end

function prologue(data)
    local name, id = data.identifier, data.id
    tabs(0)
    writeln("")
    writeln(".text")
    writeln(".globl " .. name)
    writeln(name .. ":")
    writeln(".LFP" .. id .. ":")
    tabs(1)
    writeln("pushq %rbp")
    writeln("movq %rsp, %rbp")
end

function epilogue(data)
    local name, id = data.identifier, data.id
    tabs(0)
    writeln(".LFE" .. id .. ":")
    tabs(1)
    writeln("movq %rdi, %rax")
    writeln("leave")
    writeln("ret")
    tabs(0)
    writeln(".size " .. name .. ", .LFE" .. id .. " - " .. name)
end

function call(data)
    local label = data.label
    writeln("call " .. label)
end

function collect_ret(data)
    local pos = data.pos
    writeln("movq %rax, -" .. pos .. "(%rbp)")
end

function store(data)
    local lea, src, register, pos, mov = data.lea, data.src, data.register, data.pos, data.mov
    if lea == 0 then writeln(mov .. " " .. src .. ", " .. register) end
    if lea ~= 0 then writeln("leaq " .. src .. ", " .. register) end

    writeln(mov .. " " .. register .. ", -" .. pos .. "(%rsp)")
end
