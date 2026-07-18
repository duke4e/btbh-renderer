# Održivi r3d renderer fork za Be The Bullet Hell

Status: odobren arhitekturni dizajn

Datum: 2026-07-18

Opseg: interni renderer i migracija svih rendererskih launch modova igre

## 1. Odluka

Gradimo novi mali C renderer uz postojeći r3d, namijenjen isključivo projektu Be The Bullet Hell. Novi renderer neće čuvati kompatibilnost s javnim r3d ili raylib API-jem. Stari r3d ostaje samo referentni renderer dok novi put ne prođe vizualne, funkcionalne i performance gateove; zatim se brišu stari r3d, raylib, rlgl i OpenGL runtime ovisnosti.

Platformski temelj je SDL3. GPU implementacija je jedan interni adapter iznad SDL_GPU-a:

- macOS koristi native Metal backend;
- Windows koristi Vulkan backend;
- D3D12 se ne bira kao tihi fallback;
- MoltenVK nije početni put i ne dodaje se uz SDL_GPU.

Ako vertikalni SDL_GPU spike ne može reproducirati obveznu funkciju ili mjerena regresija ostane neprihvatljiva nakon GPU analize, ista scena i isti testovi uspoređuju bgfx, direktni Vulkan + native Metal te direktni Vulkan + MoltenVK. MoltenVK je prihvatljiv kandidat samo ako njegov macOS rezultat ostane unutar 5% native Metal medijana i p95 GPU vremena, bez pipeline/shader hitchinga i bez feature regresije. Ne postoji vjerodostojan univerzalni podatak da je MoltenVK "nekoliko posto sporiji"; odluku donosimo mjerenjem ove igre.

Radiance Cascades (RC) ulazi tek nakon stabilnog feature i približnog vizualnog pariteta. Prva verzija je PoE2-style screen-space pristup nad 3D G-bufferom, u ovom projektu nazvan "2.5D RC". Ne gradimo puni world-space 3D RC u prvom izdanju.

## 2. Ciljevi i release ugovor

### 2.1 Ciljevi

1. Ukloniti raylib, rlgl i OpenGL iz shipping runtimea.
2. Održavati jedan rendererski model koji radi preko native Metala i Vulkana.
3. Zadržati sve stvarno korištene funkcije igre i alata, bez portanja neiskorištenog r3d opsega.
4. Uvesti stabilan, mjerljiv screen-space diffuse GI kroz RC.
5. Omogućiti determinističke kutove kamere po tipu igrača, uz promjenu samo iza level-up kartica.
6. Postaviti module i seamove tako da GPU backend, shader toolchain i asset import mogu evoluirati bez novog reza kroz Swift igru.

### 2.2 Podržani launch modovi

Svaki release gate vrijedi za svih pet postojećih načina pokretanja:

- glavna igra;
- combat lab;
- particle editor;
- city-block editor;
- unified-city generator/editor.

Migracija nije završena ako jedan od tih modova još treba legacy renderer.

### 2.3 Platforme i minimalni performance target

Prvi release target su macOS/Metal i Windows/Vulkan. Referentni minimum je:

- Apple M1 za Metal;
- GTX 1060 ili RX 580 klasa za Vulkan;
- 1920×1080;
- p95 ukupno prezentirano frame vrijeme najviše 16,67 ms u odobrenim reprezentativnim scenama;
- RC High cilj: najviše 3,0 ms medijan i 4,0 ms p95;
- bez vidljivog pipeline compilation hitchinga tijekom gameplaya.

Performance se mjeri nakon warm-upa, s istim scenama, kamerom, assetima, rezolucijom i quality postavkama. Vsync se isključuje za profiliranje. Windows CPU i ostatak referentnog stroja bilježe se i zamrzavaju u Phase 0. Rezultati uvijek bilježe hardware, OS, driver, commit, backend, quality tier, CPU submit vrijeme, dostupno GPU vrijeme, prezentirano frame vrijeme, medijan i p95.

### 2.4 Vizualni paritet

Prije RC-a novi renderer mora postići:

- sve gameplay i editor funkcije postojećeg renderera;
- SSIM najmanje 0,98 prema ručno odobrenim golden screenshotovima za stabilne scene;
- nijedan nestali gameplay cue unutar posebno označenih maski;
- ručni pregled prve izmjene svakog goldena.

Pixel-identičnost nije cilj jer se Metal/Vulkan rasterizacija, texture filtering i novi G-buffer mogu legitimno razlikovati. Čitljivost gameplaya, materijali, sjene, animacija, UI i namjerni post efekti jesu obvezni.

## 3. Činjenice iz postojećeg koda

Audit trenutačnog r3d repozitorija i priložene igre pokazao je:

- približno 27.750 linija C/header koda u r3d-u;
- 33 r3d source datoteke, približno 17.393 linija, direktno su vezane uz OpenGL;
- 24 od 33 javna r3d headera uključuju raylib;
- igra uključuje raylib u 82 od 186 pregledanih Swift datoteka i koristi približno 83 raylib funkcije, uz njegove math tipove;
- vendani r3d headeri igre razlikuju se od trenutačnog repozitorija: igra koristi stariji ili lokalno prilagođen ABI, uključuje `r3d_visibility`, nema `r3d_camera` i `r3d_pack`, a više zajedničkih headera nije jednako;
- SDL3 je već prisutan, primarno za gamepad;
- trenutačni G-buffer ima šest color attachmenta, dok SDL_GPU render pass dopušta najviše četiri;
- igra koristi SSAO, directional i spot svjetla sa sjenama, proceduralno nebo i cubemap IBL, prilagođene surface shadere, LUT, CAS i tilt-shift screen shadere;
- igra koristi mesh/model/material, instancing, skeletal animation player/tree i runtime Assimp import;
- decals, reflection-probe capture, kinematics, pack, custom sky shader, SSIL, SSR, DoF, volumetric fog, auto exposure i bloom nisu dio shipping puta koji treba prenijeti;
- postojeći `cameraGlitch` kontinuirano mijenja yaw, pitch i udaljenost, ali već postoji level-up snapshot/card prijelaz koji može sakriti diskretnu promjenu kamere.

Posljedica je jasna: ovo nije zamjena OpenGL poziva jedan-za-jedan. Drop-in shim bi sačuvao najgore postojeće sprege i ipak zahtijevao prepisivanje većine renderera. Novi duboki modul s malim projektnim interfaceom ima manji dugoročni trošak.

Detaljni izvori i tehnička ograničenja nalaze se u [istraživanju rendererskih backenda](../../architecture/rendering_backend_research.md).

## 4. Arhitekturni rez

```text
main game / combat lab / particle editor / city editors
                         │
                         ▼
project public seam: POD tipovi + opaque handleovi + frame/scene naredbe
                         │
          ┌──────────────┼──────────────┐
          ▼              ▼              ▼
 scene submission   frame graph     Canvas2D
 instancing         passes          text/images/UI
 animation          resources
          └──────────────┬──────────────┘
                         ▼
        minimalni interni GPU seam iz stvarnih passova
                         ▼
                   SDL_GPU adapter
                         ▼
             Metal (macOS) / Vulkan (Windows)
```

SDL tipovi, SDL_GPU objekti, command bufferi, native handleovi i sinkronizacija ne prelaze javni projektni seam. Time SDL_GPU ostaje zamjenjiv interni adapter, a Swift igra ne postaje vezana za još jednu vanjsku grafičku biblioteku.

### 4.1 Moduli

#### Platform

Vlasnik je SDL lifecyclea, windowa, eventa, inputa, vremena, clipboarda, high-DPI i native-handle pristupa. Platform predaje rendereru apstraktni window/surface token, ne `SDL_Window*` kroz javni interface.

Audio nije dio renderera. Postojeći miniaudio put ostaje zaseban. Uklanjanje rayliba ne uvodi novu audio migraciju.

#### Renderer public seam

Jedini seam koji igra i alati koriste za 3D i 2D iscrtavanje. Izlaže:

- vlastite trivijalno kopirljive vector, matrix, color, rectangle, image-view i bounding tipove;
- generacijske opaque handleove za mesh, texture, material, model, skeleton, font i render target;
- validirane opise resursa;
- upload/update/destroy naredbe;
- immutable frame, scene, environment, camera i Canvas2D opise;
- eksplicitne quality postavke i debug view;
- strukturirane init/runtime greške i diagnostics snapshot.

Ne izlaže backend enumove osim projektnog zahtjeva `Metal` ili `Vulkan`, shader binding brojeve, raw GPU pokazivače, command encodere ni lifecycle detalje frame-grapha.

#### Scene submission

Pretvara game-owned scene opise u kompaktan immutable snapshot za trenutni frame. Vlasnik je:

- draw packet sortiranja;
- persistent instance setova i city chunkova;
- frustum cullinga;
- material/surface varijanti;
- skeletal pose uploadova;
- transparentnog redoslijeda.

Game i alati posjeduju semantičke scene i handleove. Renderer posjeduje transientne draw pakete i GPU reprezentaciju.

#### Asset

Vlasnik je CPU decode/importa i pretvorbe u validirane renderer opise. Početni parity put zadržava Assimp iza adaptera za postojeće OBJ/glTF/GLB assete. PNG, JPEG, WebP, HDR i font rasterizacija također ostaju iza uskih decoder/rasterizer interfacea, tako da format biblioteke nisu dio rendererovog javnog seama.

Nakon pariteta uvodi se offline asset pack za shipping, ali runtime Assimp se ne uklanja prije nego svi editori i razvojni workflowi imaju zamjenu. Offline format nije preduvjet GPU migracije.

#### Frame graph

Vlasnik je pass ovisnosti, transientnih tekstura/buffera, resource lifetimea i redoslijeda graphics/compute/copy rada. Graph se kompilira iz unaprijed registriranih pass predložaka; gameplay ne konstruira proizvoljan GPU graph.

Graph mora:

- odbiti ciklus prije submitanja;
- izračunati prvi i zadnji resource use;
- aliasirati kompatibilne transientne resurse kada je sigurno;
- eksplicitno opisati read/write usage;
- ponuditi pass i resource debug oznake za GPU capture.

#### PBR/shadows/post

Duboki render modul koji implementira odobreni pass redoslijed, pipeline varijante i quality tierove. Stara OpenGL state cache logika ne prelazi ovaj seam; SDL_GPU adapter dobiva immutable pipeline i resource opise.

#### Canvas2D

Batched 2D/text/image renderer zamjenjuje raylib 2D pozive. Podržava sprite/image quadove, solid/gradient primitive koje igra stvarno koristi, scissor, transform, blend mode, font atlas, text measurement i stabilan draw order. UI se kompozitira nakon 3D/post obrade, uključujući level-up snapshot i kartice.

Canvas2D nije generički retained-mode UI framework. Swift igra zadržava svoj UI state i layout.

#### Radiance Cascades

Opcionalan frame-graph modul s vlastitim quality tierovima, debug prikazima i confidence izlazom. Ne utječe na ispravnost scene: ako nije dostupan, renderer koristi definirani IBL + ambient + SSAO fallback.

#### CameraProfile

Mali deterministički game modul, ne GPU backend funkcija. Bira profil iz tipa igrača, run seeda i levela te ga primjenjuje isključivo u level-up transakciji.

## 5. Vlasništvo i lifetime

### 5.1 Resursi

Renderer posjeduje GPU device, swapchain, pipelineove, GPU memory, frame graph i GPU kopije asseta. Igra posjeduje handle vrijednosti i CPU semantiku.

Svi javni handleovi su 64-bitni `index + generation`. Stari handle nakon destroy/reuse operacije ne može slučajno referencirati novi resurs. Invalid handle daje strukturiranu grešku u validation buildu i siguran no-op ili fallback resurs u release buildu; ne uzrokuje use-after-free.

Destroy je odgođen do fencea posljednjeg framea koji koristi resurs. Upload staging i transientni CPU podaci dolaze iz frame-local ring arene. Render loop ne radi opće heap alokacije nakon warm-upa. Persistent instance bufferi se ažuriraju parcijalno, umjesto ponovnog stvaranja svakog framea.

### 5.2 Threading

Prva verzija koristi jedan render thread. Game thread sastavlja scene i frame opise, a render thread ih konzumira kao immutable snapshot. Asset decode može raditi u pozadini; stvaranje GPU resursa ulazi u thread-safe upload queue koji se prazni na početku framea.

Ne uvodi se paralelno command encoding dok profiler ne pokaže CPU bottleneck. Interface ne ovisi o toj odluci, pa se threading može produbiti kasnije bez promjene caller koda.

### 5.3 Frame lifecycle

Jedan frame prolazi ovim redoslijedom:

1. Platform obradi evente, input, resize i high-DPI promjenu.
2. Renderer rezervira sljedeći frame slot i pričeka samo njegov fence ako je nužno.
3. Upload queue se prazni kroz copy pass.
4. Game predaje immutable camera, environment, scene i Canvas2D snapshot.
5. Scene submission radi culling, sortiranje i draw packet kompilaciju.
6. Frame graph izvršava 3D, optional RC i post passove.
7. Canvas2D kompozitira UI, tekst i card/snapshot overlay.
8. Command buffer se submita, presentira i veže uz fence frame slota.
9. Oslobađaju se resursi čiji je zadnji fence završen.

Resize suspendira present samo dok swapchain nema valjanu veličinu. GPU resursi koji ne ovise o swapchainu ostaju živi.

## 6. GPU i shader seam

Interni GPU seam projektira se iz stvarnih passova igre, ne kao preslika Vulkana ili Metala. Mora pokriti:

- device/swapchain capability query;
- buffer i texture create/upload/copy;
- graphics i compute pipeline create;
- render, compute i copy pass encoding;
- sampled/storage texture i buffer binding;
- uniform/push podatke u granicama SDL_GPU modela;
- indexed, instanced i indirect draw gdje stvarni pass to koristi;
- dispatch, fence, labels i capture-friendly debug grupe.

Backend se bira eksplicitno pri inicijalizaciji. Ako zatraženi Metal ili Vulkan backend, format, storage usage ili compute mogućnost nije dostupna, init završava jasnim izvještajem. Ne prelazi tiho na D3D12 ili legacy OpenGL.

### 6.1 Shader build

Production build nema runtime GLSL kompilaciju. Shader pipeline je:

1. kanonski shader source;
2. offline kompilacija u SPIR-V;
3. SPIR-V za Vulkan;
4. offline generirani MSL i `metallib` za Metal;
5. reflection manifest s resource layoutom, vertex inputom, specialization vrijednostima i hashom;
6. build-time validacija da C opisi i reflection manifest odgovaraju.

Postojeći GLSL može biti početni source tijekom migracije. Odluka o budućem prelasku na HLSL donosi se tek nakon pariteta; nije dio GPU spikea. Shader ili reflection mismatch ruši build, a ne odgađa grešku do runtimea.

Sve surface varijante koje igra stvarno koristi postaju offline pipeline varijante. Proizvoljno učitavanje runtime GLSL-a ne ulazi u novi public interface.

## 7. Render pipeline

### 7.1 Redoslijed passova

1. Directional i spot shadow atlas.
2. Opaque i alpha-clip G-buffer.
3. Optional half-resolution SSAO.
4. Deferred direct lighting + cubemap IBL.
5. Optional RC diffuse indirect.
6. Sky te transparentni i unlit forward passovi.
7. SMAA, LUT, CAS i tilt-shift post lanac prema aktivnim game postavkama.
8. Canvas2D/UI i present.

Bloom, SSR, SSIL/stari SSGI, DoF, volumetric fog i auto exposure nisu skrivene inactive grane u novom graphu. Ako se u budućnosti vrate kao stvarna potreba igre, ulaze kroz zaseban dizajn i mjerljiv pass contract.

### 7.2 Novi G-buffer

Šest starih color attachmenta zamjenjuju tri:

| Target | Format | Sadržaj |
| --- | --- | --- |
| G0 | `RGBA8_SRGB` | `albedo.rgb`, ambient occlusion |
| G1 | `RGBA8_UNORM` | octahedral normal `xy`, roughness, metalness |
| G2 | `RGBA16_FLOAT` | emission `rgb`, specular |
| Depth | sampleable hardware depth | depth u `[0,1]` |

World/view pozicija rekonstruira se iz deptha i inverse projection/view matrica. Geometrijska normala rekonstruira se iz depth derivacija tamo gdje je potrebna. Time pipeline ostaje ispod SDL_GPU granice od četiri color targeta, smanjuje bandwidth i uklanja redundantni linear-depth target.

Sve depth reconstruction, handedness, culling i screen-space formule imaju golden i math testove jer SDL_GPU koristi left-handed konvenciju i depth `[0,1]`.

### 7.3 Skinning, instancing i transparentnost

Skinning matrice idu u storage buffer ili 2D texture fallback, ne u 1D OpenGL teksturu. Pipeline mora podržati postojeći animation player/tree rezultat i isti bone limit koji audit potvrdi kao korišten.

Grad i ponavljani objekti koriste persistent instance setove s dirty-range updateom. CPU frustum culling ostaje početni stabilni put; GPU-driven culling ulazi samo ako mjerenje pokaže da je CPU submission bottleneck.

Alpha-clip materijali ulaze u G-buffer i sjene. Blended transparentni materijali renderiraju se forward, primaju resolved indirect, ali u prvoj RC verziji ne sudjeluju u RC visibilityju.

## 8. Portati, zamijeniti, ukloniti

### 8.1 Portati

- mesh, model, material i texture lifecycle;
- runtime Assimp parity za postojeće development/editor tokove;
- instancing, persistent city chunks i frustum culling;
- skeletal animation, player i tree;
- directional i spot svjetla, soft shadows i shadow atlas;
- procedural sky, cubemap IBL i postojeći namjerni tonemap;
- SSAO, SMAA, LUT, CAS i tilt-shift;
- korištene surface/unlit/alpha-clip varijante;
- korištene primitive generator funkcije;
- 2D slike, primitive, fontove, mjerenje teksta i UI clipping;
- offscreen snapshot potreban za level-up kartice.

### 8.2 Zamijeniti

- raylib math/color/image/window/input tipove projektnim POD tipovima i Platform seamom;
- raw environment pokazivače validiranim immutable frame/environment opisom;
- runtime GLSL offline shader varijantama i reflectionom;
- ručno ponovno korištenje buffera frame arenom, upload queueom i fence lifetimeom;
- raylib 2D/text batched Canvas2D modulom;
- OpenGL state cache immutable pipeline objektima;
- kontinuirani `cameraGlitch` diskretnim `CameraProfile` prijelazom iza kartica.

### 8.3 Ne portati

- OpenGL driver, rlgl i raylib compatibility surface;
- decals i reflection-probe capture;
- kinematics/collision helper sloj;
- r3d pack;
- custom sky shader sustav;
- SSIL/stari SSGI, SSR, DoF i volumetric fog;
- auto exposure, bloom i druge disabled post grane;
- neiskorištene primitive i debug helper funkcije;
- javnu upstream r3d ABI kompatibilnost.

Popis se provjerava statičkim usage scanom igre prije svakog removal koraka. "Ne portati" znači ukloniti nakon legacy gašenja, ne držati mrtvi kod iza build flaga.

## 9. Radiance Cascades contract

### 9.1 Namjena i granice

RC daje dinamički diffuse indirect za kontroliranu perspektivu igre. Scene su 3D, ali visibility i cascade polje dolaze iz trenutnog screen-space G-buffera. Početni milestone ima jednu diffuse bounce komponentu; specular RC i puni world-space volume nisu u opsegu.

RC nije autoritativan gameplay signal. Kritični projektil, opasnost, izlaz, interakcija ili neprijatelj mora imati direct/emissive/UI cue koji ostaje vidljiv kada je RC Off.

### 9.2 Ulazi i izlazi

Ulazi:

- depth;
- shading i rekonstruirana geometrijska normala;
- albedo i material parametri;
- emission;
- direct diffuse rezultat;
- camera/projection podaci;
- quality tier i debug izbor.

Izlazi:

- HDR diffuse indirect;
- confidence po pikselu;
- optional debug texture za cascade interval, hit distance i fallback razlog.

### 9.3 Passovi

1. Visible emissive i direktno osvijetljene površine pune screen-space radiance seed.
2. Svaka kaskada radi distance-bounded screen-space interval trace u svojem prostornom i kutnom mjerilu.
3. Kaskade se spajaju od najdalje prema najbližoj.
4. Depth/normal-aware resolve interpolira radiance i računa confidence.
5. Compose dodaje diffuse indirect na HDR lighting i prema confidenceu blendira prema IBL + ambient fallbacku.

Baseline rekonstruira rezultat svakog framea bez obveznog temporal accumulationa. Time fiksna kamera daje stabilnost bez history ghostinga, a diskretni camera transition može eksplicitno invalidirati sve RC podatke.

### 9.4 Quality tierovi

- `High`: ciljna kvaliteta, RC p95 do 4,0 ms na referentnom minimumu.
- `Low`: manja interna rezolucija/broj intervala uz iste semantičke izlaze.
- `Off`: IBL + ambient + SSAO, bez RC resursa i dispatcha.
- `Auto`: bira između prethodna tri na temelju zagrijanog rolling p95 frame-completion vremena, s histerezom. Ako adapter ima pouzdano GPU vrijeme, ono je dodatni signal, ali nije preduvjet jer ga SDL_GPU javni interface trenutačno ne izlaže.

`Auto` ne mijenja tier usred levela. Odluka i eventualna reallocacija primjenjuju se iza level-up kartica zajedno s CameraProfile transakcijom.

### 9.5 Definirani artefakti i fallback

- Off-screen emitter ne doprinosi RC-u; kritičan izvor zato mora imati direct ili vidljiv emissive cue.
- Nevidljivi okluder snižava confidence; resolve blendira prema IBL/ambientu i nikad ne proizvodi crnu rupu.
- Geometrija iza depth buffera nije dostupna RC-u.
- Transparentni objekti primaju resolved indirect, ali ne ulaze u visibility u prvoj verziji.
- RC init/dispatch problem isključuje RC, bilježi razlog i nastavlja s IBL + ambient + SSAO.

World-space/hybrid dodatak dopušten je tek ako reprezentativne scene pokažu ponovljiv gameplay-relevantan nedostatak koji confidence/fallback ne rješava.

### 9.6 Debuggability

Obvezni debug prikazi su:

- pojedinačna cascade interval polja;
- traced hit distance;
- confidence;
- finalni indirect;
- fallback reason;
- RC GPU vrijeme po passu kada backend tooling to omogućuje.

Metal i Vulkan koriste isti compute raspored, reflection manifest i semantičke testove. Backend-specifičan shader može postojati samo kao dokazani workaround unutar adaptera, uz isti izlazni contract i zaseban golden.

## 10. CameraProfile transakcija

Postojeći gameplay ostaje na fiksnoj kameri unutar levela. Novi profil određuje:

- yaw;
- pitch;
- distance;
- opcionalni mali framing offset;
- profil ID za diagnostics i replay.

Odabir je čista deterministička funkcija `player type + run seed + level`. Isti replay input mora dati isti profil. Profil je content data i može se art-tunirati bez promjene interfacea, ali ne smije prijeći već korišteni `cameraGlitch` safety envelope: yaw najviše ±0,55 rad od baze, pitch offset od -0,20 do +0,14 rad i distance faktor od 0,78 do 1,26. Svaki shipping profil dodatno mora proći gameplay cue maske; cilj nije proizvoljna slobodna kamera.

Promjena se izvršava atomarno:

1. game zadrži zadnji gameplay frame kao snapshot;
2. prikažu se roguelike kartice;
3. odabere se novi CameraProfile;
4. primijene se yaw, pitch, distance i framing;
5. invalidiraju se RC/history, visibility/culling cache i camera-dependent post podaci;
6. renderer odradi skriveni warm-up frame iza kartica;
7. nova scena se otkrije tek nakon zatvaranja kartica.

Camera shake ostaje mali transientni view offset i ne mijenja bazni CameraProfile. Stari višesekundni kontinuirani `cameraGlitch` se uklanja. Budući tipovi igrača mogu dobiti različite familije profila bez promjene rendererovog interfacea.

## 11. Migracija bez big-bang reza

Ovaj dokument je program-level arhitekturni contract. Izvedba se dijeli u odvojene planove i review gateove; sljedeći implementation plan pokriva samo Phase 0 i Phase 1.

### Phase 0 — baseline

- automatski inventar korištenih raylib/r3d simbola;
- reprezentativne scene i deterministic capture ulazi za svih pet modova;
- golden screenshotovi i gameplay cue maske;
- CPU/GPU baseline na referentnom Metalu i Vulkan/OpenGL usporedbi gdje je dostupna;
- asset, resize/high-DPI i shutdown smoke testovi.

Izlaz: ponovljiv dokaz što mora ostati isto i koliko trenutačni renderer košta.

### Phase 1 — game seamovi

- projektni math/POD tipovi;
- Platform interface iznad SDL3;
- renderer public seam i handle registry;
- Canvas2D interface;
- legacy raylib/r3d adapter koji održava postojeću igru funkcionalnom.

Izlaz: game i alati više ne govore direktno s raylib/r3d interfaceom, ali legacy renderer još crta.

### Phase 2 — SDL_GPU vertikalni spike

- SDL_GPU device/swapchain na Metal i Vulkan backendu;
- offline SPIR-V/`metallib` build i reflection;
- novi G-buffer i minimalni PBR;
- directional/spot shadow;
- skeletal skinning bez 1D teksture;
- instanced opaque, jedan transparentni pass i compute read/write test;
- GPU capture i mjerenje na oba backenda.

Spike prolazi samo ako obvezni format/storage/compute featurei rade na M1 i GTX 1060/RX 580 klasi, reflection je determinističan, a nijedan portability workaround ne curi kroz public seam.

### Phase 3 — feature paritet

- puni instancing/culling;
- animation player/tree;
- IBL, SSAO, sky i korištene surface varijante;
- SMAA/LUT/CAS/tilt-shift;
- asset import, images, fonts i Canvas2D;
- golden i performance paritet glavne igre.

### Phase 4 — svi alati i launch modovi

- combat lab;
- particle editor;
- city-block editor;
- unified-city;
- resize/high-DPI, asset lifetime i clean shutdown na oba OS-a.

### Phase 5 — RC i CameraProfile

- RC High/Low/Off/Auto;
- confidence/fallback/debug contract;
- CameraProfile determinism i hidden transition;
- cross-backend golden/performance testovi.

### Phase 6 — removal

- uklanjanje legacy adaptera;
- brisanje starog r3d OpenGL drivera i shader binarija;
- brisanje raylib i rlgl source/include/link ovisnosti;
- brisanje funkcija označenih kao "ne portati";
- dependency scan potvrđuje nula runtime linkova i source includeova rayliba, rlgl-a i OpenGL-a, osim povijesne dokumentacije.

Legacy build/runtime izbor ostaje do kraja Phase 5. Nakon Phase 6 rollback je git revert provjerenog removal commita; ne održava se trajni compatibility shim.

## 12. Gateovi, testovi i failure politika

### 12.1 Unit testovi

- handle generation, stale-handle i delayed-destroy;
- frame arena granice i alignment;
- frame-graph cycle detection i resource lifetime;
- projection/depth reconstruction/culling math;
- CameraProfile determinism i dozvoljeni raspon;
- RC tier state machine i hysteresis;
- descriptor/reflection hash validacija.

### 12.2 Build-time testovi

- shader compile za SPIR-V i `metallib`;
- reflection/C layout podudaranje;
- zabranjeni dependency scan;
- svi launch modovi linkaju bez implicitnog backenda;
- reproducible asset/shader manifest hash.

### 12.3 GPU integration testovi

- device init i točan backend;
- resize, minimize/restore i high-DPI;
- upload/update/destroy pod opterećenjem;
- opaque, alpha-clip, transparent, shadow i skinning referentne scene;
- compute write → sample read hazard;
- screenshot readback;
- controlled shutdown bez validation grešaka.

### 12.4 Release gate

Svaka faza zahtijeva green unit/build/GPU testove, ručno odobrene visual diffove i performance izvještaj. Regresija veća od 5% traži GPU capture i objašnjenje prije mergea, čak i kada frame ostaje ispod 16,67 ms.

Failure politika:

- nedostaje obvezni Metal/Vulkan feature: renderer init ne uspijeva s eksplicitnim capability reportom;
- RC nije dostupan ili faila: log + IBL/ambient/SSAO fallback;
- shader/reflection mismatch: build fail;
- device loss u v1: kontrolirani izlaz s diagnostics dumpom; live recovery nije dio v1;
- invalid game resource handle: validation error, siguran fallback u release buildu.

## 13. Rizici i mitigacije

| Rizik | Mitigacija |
| --- | --- |
| SDL_GPU je mlad i nema svaki low-level feature | Rani vertikalni spike, capability gate i backend-neutralan javni seam |
| G-buffer prepakiranje promijeni izgled | Novi 3-target layout, math testovi i odobreni goldeni |
| Raylib je raširen kroz Swift kod | Phase 1 legacy adapter i usage scan; nema big-bang migracije |
| Vendani r3d ABI nije jednak ovom repozitoriju | Migrira se stvarni game usage, ne pretpostavljena upstream kompatibilnost |
| Shader cross-compile stvara runtime hitch | Offline build, `metallib`, reflection i pipeline cache/warm-up |
| Screen-space RC gubi off-screen podatke | Confidence, IBL fallback, obvezni direct/emissive gameplay cueovi |
| Promjena kamere razotkrije RC/disocclusion artefakte | Promjena samo iza kartica, potpuna invalidacija i hidden warm-up |
| Dva renderera predugo koegzistiraju | Faze imaju removal gate; legacy se briše odmah nakon Phase 5 prolaza |
| Toolovi ostanu na legacy putu | Paritet nije release-ready dok svih pet launch modova ne prođe |
| Performance broj izgleda dobro samo na jednom stroju | Fiksna metodologija, reference klase, medijan+p95 i capture za >5% regresiju |

## 14. Ne-ciljevi

- general-purpose engine ili novi javni r3d projekt;
- kompatibilnost sa svim raylib/r3d korisnicima;
- Linux release u prvom migracijskom ciklusu;
- D3D12 backend;
- ray tracing ili mesh shader zahtjevi;
- puni world-space 3D Radiance Cascades;
- RC specular u prvom milestoneu;
- retained-mode UI framework;
- live GPU device-loss recovery u v1;
- offline asset pipeline kao preduvjet rendererskog pariteta.

## 15. Prihvaćena krajnja slika

Nakon Phase 6 igra i svi editori koriste projektni renderer seam, SDL3 Platform i SDL_GPU adapter. macOS izvršava native Metal, Windows Vulkan. Shipping build ne sadrži raylib, rlgl, OpenGL ni mrtve r3d mogućnosti. Renderer zadržava samo module koje igra koristi, RC se može sigurno ugasiti bez gubitka gameplay čitljivosti, a CameraProfile se mijenja deterministički samo iza level-up kartica.

To je održivi fork: ne pokušava spasiti stari javni API, ne održava dva native backenda i ne veže igru za odabranu GPU biblioteku.
