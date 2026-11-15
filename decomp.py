import json,sys
p=json.load(open(sys.argv[1]))
out=open(sys.argv[2],'w')
def inp(blocks,b,name):
    i=b['inputs'].get(name)
    if not i: return '_'
    v=i[1]
    if v is None: return '_'
    if isinstance(v,str): return expr(blocks,v)
    if isinstance(v,list):
        if v[0] in (4,5,6,7,8,9,10): return repr(v[1])
        if v[0]==11: return 'bcast('+repr(v[1])+')'
        if v[0]==12: return 'var['+v[1]+']'
        if v[0]==13: return 'list['+v[1]+']'
    return str(v)
def fld(b,name):
    f=b['fields'].get(name); return f[0] if f else '?'
def expr(blocks,bid):
    b=blocks[bid]; op=b['opcode']
    if op=='procedures_prototype': return ''
    if op=='argument_reporter_string_number': return 'arg['+fld(b,'VALUE')+']'
    if op=='argument_reporter_boolean': return 'argb['+fld(b,'VALUE')+']'
    if op.startswith('operator_'):
        n=op[9:]
        bin={'add':'+','subtract':'-','multiply':'*','divide':'/','lt':'<','gt':'>','equals':'==','and':'and','or':'or','mod':'mod'}
        if n in bin:
            k=list(b['inputs'].keys())
            return '('+inp(blocks,b,k[0])+' '+bin[n]+' '+inp(blocks,b,k[1] if len(k)>1 else '')+')'
        if n=='not': return 'not('+inp(blocks,b,'OPERAND')+')'
        if n=='mathop': return fld(b,'OPERATOR')+'('+inp(blocks,b,'NUM')+')'
        if n=='random': return 'random('+inp(blocks,b,'FROM')+','+inp(blocks,b,'TO')+')'
        if n=='round': return 'round('+inp(blocks,b,'NUM')+')'
        if n=='join': return 'join('+inp(blocks,b,'STRING1')+','+inp(blocks,b,'STRING2')+')'
        if n=='letter_of': return 'letter('+inp(blocks,b,'LETTER')+','+inp(blocks,b,'STRING')+')'
        if n=='length': return 'len('+inp(blocks,b,'STRING')+')'
        if n=='contains': return 'contains('+inp(blocks,b,'STRING1')+','+inp(blocks,b,'STRING2')+')'
    if op=='data_variable': return 'var['+fld(b,'VARIABLE')+']'
    if op=='data_itemoflist': return 'list['+fld(b,'LIST')+']['+inp(blocks,b,'INDEX')+']'
    if op=='data_lengthoflist': return 'len(list['+fld(b,'LIST')+'])'
    if op=='data_itemnumoflist': return 'indexof(list['+fld(b,'LIST')+'],'+inp(blocks,b,'ITEM')+')'
    if op=='data_listcontainsitem': return 'contains(list['+fld(b,'LIST')+'],'+inp(blocks,b,'ITEM')+')'
    if op.endswith('_menu') or op in ('looks_costume','looks_backdrops','sound_sounds_menu','control_create_clone_of_menu','sensing_touchingobjectmenu','sensing_keyoptions','sensing_of_object_menu','motion_goto_menu','pen_menu_colorParam','sensing_distancetomenu'):
        return repr(list(b['fields'].values())[0][0])
    args=[]
    for k in b['inputs']: args.append(k+'='+inp(blocks,b,k))
    for k in b['fields']: args.append(k+':'+str(b['fields'][k][0]))
    return op+'('+', '.join(args)+')'
def stmt(blocks,bid,ind):
    while bid:
        b=blocks[bid]; op=b['opcode']; pad='  '*ind
        if op=='control_if':
            out.write(pad+'if '+inp(blocks,b,'CONDITION')+':\n'); sub(blocks,b,'SUBSTACK',ind+1)
        elif op=='control_if_else':
            out.write(pad+'if '+inp(blocks,b,'CONDITION')+':\n'); sub(blocks,b,'SUBSTACK',ind+1); out.write(pad+'else:\n'); sub(blocks,b,'SUBSTACK2',ind+1)
        elif op in('control_forever',):
            out.write(pad+'forever:\n'); sub(blocks,b,'SUBSTACK',ind+1)
        elif op=='control_repeat':
            out.write(pad+'repeat '+inp(blocks,b,'TIMES')+':\n'); sub(blocks,b,'SUBSTACK',ind+1)
        elif op=='control_repeat_until':
            out.write(pad+'repeat until '+inp(blocks,b,'CONDITION')+':\n'); sub(blocks,b,'SUBSTACK',ind+1)
        elif op=='control_while':
            out.write(pad+'while '+inp(blocks,b,'CONDITION')+':\n'); sub(blocks,b,'SUBSTACK',ind+1)
        elif op=='data_setvariableto': out.write(pad+'var['+fld(b,'VARIABLE')+'] = '+inp(blocks,b,'VALUE')+'\n')
        elif op=='data_changevariableby': out.write(pad+'var['+fld(b,'VARIABLE')+'] += '+inp(blocks,b,'VALUE')+'\n')
        elif op=='procedures_definition':
            pr=blocks[b['inputs']['custom_block'][1]]['mutation']
            out.write(pad+'define '+pr['proccode']+' '+pr['argumentnames']+' warp='+pr.get('warp','')+'\n')
        elif op=='procedures_call':
            m=b['mutation']; ids=json.loads(m['argumentids'])
            out.write(pad+'call '+m['proccode']+'('+', '.join(inp(blocks,b,i) for i in ids)+')\n')
        else: out.write(pad+expr(blocks,bid)+'\n')
        bid=b['next']
def sub(blocks,b,n,ind):
    i=b['inputs'].get(n)
    if i and i[1]: stmt(blocks,i[1],ind)
for t in p['targets']:
    out.write('\n########## '+t['name']+'\n')
    bl=t['blocks']
    for bid,b in bl.items():
        if isinstance(b,dict) and b.get('topLevel') and b['opcode'] not in ('procedures_prototype',) and not b['opcode'].startswith('data_') or (isinstance(b,dict) and b.get('topLevel') and b['opcode']=='procedures_definition'):
            if b['opcode'].endswith('_menu'): continue
            out.write('\n')
            stmt(bl,bid,0)
