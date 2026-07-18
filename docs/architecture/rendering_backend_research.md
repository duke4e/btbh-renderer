# Istraživanje održivog rendererskog forka

Status izvora: 2026-07-18. Ova bilješka koristi službenu dokumentaciju, izvorne repozitorije i radove autora. Zaključci označeni kao *inference* su arhitektonske procjene izvedene iz tih izvora i trenutačnog r3d koda.

## Sažetak odluke

1. **Koristiti SDL3, ne SDL2.** SDL2 više nema planirane releaseove osim mogućih kritičnih popravaka, a SDL izričito preporučuje SDL3 za novi razvoj. SDL3 je službeno objavljen i aktivna je stabilna linija; aktualni stabilni patch na datum ove bilješke je 3.4.12. ([SDL2 status](https://wiki.libsdl.org/SDL2/FrontPage), [SDL3](https://wiki.libsdl.org/SDL3/FrontPage), [3.4.12](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.12))
2. **Kao prvi renderer koristiti SDL_GPU iz SDL3**, s forsiranim Vulkan backendom na Windowsu/Linuxu i Metal backendom na Apple platformama. SDL_GPU je C API za moderne graphics i compute pipelineove te službeno ima Vulkan, Metal i D3D12 backende. ([SDL_GPU](https://wiki.libsdl.org/SDL3/CategoryGPU))
3. **Ne pisati vlastiti Vulkan i Metal backend u prvoj fazi i ne uvoditi MoltenVK uz SDL_GPU.** Vlastiti backendi imaju smisla samo ako se potvrdi potreba za mogućnostima koje SDL_GPU namjerno ne pokriva, primjerice ray tracingom ili mesh shaderima. SDL kaže da takve mogućnosti nisu u bliskom planu. ([SDL_GPU FAQ](https://wiki.libsdl.org/SDL3/CategoryGPU#faq))
4. **Radiance Cascades započeti kao PoE2-style screen-space, odnosno projektni „2.5D” pristup.** Autor algoritma i GGG-ov Senior Graphics Programmer navodi da Path of Exile 2 koristi flatland-like screen-space cascade hierarchy popunjenu screen-space raymarchingom zato što igra ima fiksnu kameru. To je direktno relevantnije od čistog 2D ili punog world-space 3D pristupa. ([izvorni rukopis](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex), [GGG najava predavanja](https://www.pathofexile.com/forum/view-thread/3447463/filter-account-type/staff))
5. **Ovo nije zamjena OpenGL poziva jedan-za-jedan.** Sadašnji r3d javni API izlaže raylib tipove, renderer je direktno vezan za OpenGL state, a G-buffer koristi šest color attachmenta. SDL_GPU dopušta najviše četiri color targeta po render passu. Potreban je mali novi renderer core i prepakiranje G-buffera, ne kompatibilnostni shim oko postojećeg drivera. ([r3d public core](../../include/r3d/r3d_core.h), [r3d G-buffer](../../src/modules/r3d_target.h), [SDL_GPU pass model](https://wiki.libsdl.org/SDL3/CategoryGPU))

## 1. SDL2 ili SDL3 kao platform layer

### Dokazi

- Službena SDL2 dokumentacija kaže da nema daljnjih planiranih SDL2 releaseova; mogu se prihvatiti pull requestovi i izdati kritični popravci. Za novi razvoj preporučuje SDL3. ([SDL2 future support](https://wiki.libsdl.org/SDL2/FrontPage#future-support))
- SDL3 je službeno objavljen, pisan je u C-u i pokriva prozore, input, audio i low-level pristup Vulkanu i Metalu. ([SDL3 front page](https://wiki.libsdl.org/SDL3/FrontPage))
- SDL3 nije source-compatible nadogradnja SDL2: promijenjeni su headeri, povratne vrijednosti, simboli, event modeli i build targeti. Službeni vodič daje rename skripte i Coccinelle patch za migraciju. ([SDL2→SDL3 migration](https://wiki.libsdl.org/SDL3/README-migration))
- `sdl2-compat` je koristan za održavanje postojećih SDL2 aplikacija, ali njegov vlastiti README za novi kod preporučuje direktni SDL3; kompatibilnost nije potpuna kada aplikacija direktno pristupa native window sustavu. ([sdl2-compat](https://github.com/libsdl-org/sdl2-compat/blob/main/README.md))

### Zaključak

SDL2 nema održivu prednost za novi fork. SDL3 treba biti jedini platform target. Treba pinati stabilni SDL3 tag i nadograđivati ga kontrolirano kroz CI, a ne pratiti `main`; službena politika verzioniranja navodi stabilne production releaseove i backward compatibility novijih stabilnih verzija. ([SDL3 versioning](https://wiki.libsdl.org/SDL3/README-versions))

Platform layer treba ostati uzak: lifecycle, window, input, time, clipboard i native handles. Ne treba ponoviti sadašnju raylib spregu tako da SDL tipovi postanu rendererov javni model. r3d trenutačno javno izlaže `Vector*`, `Matrix`, `Color`, `Image`, `Texture`, `BoundingBox` i druge raylib tipove kroz headere kao što su [core](../../include/r3d/r3d_core.h), [mesh](../../include/r3d/r3d_mesh.h) i [texture](../../include/r3d/r3d_texture.h). *Inference:* novi fork treba vlastite POD math/color/image opise i opaque GPU handleove; SDL implementacija ostaje iza internog adaptera.

## 2. Vulkan i Metal opcije

### Opcija A — SDL3 + SDL_GPU: preporuka

SDL_GPU izlaže graphics i compute pipelineove, command buffere, render/compute/copy passove, storage buffere/teksture, indirect draw/dispatch, swapchain i fences. Vulkan backend službeno radi na Windowsu i Linuxu, a Metal backend na macOS-u i Apple mobile platformama. ([SDL_GPU API i zahtjevi](https://wiki.libsdl.org/SDL3/CategoryGPU))

Prednosti za ovaj fork:

- jedan C render API i jedan resource/pipeline model za Vulkan i native Metal;
- ne treba održavati dvije sinkronizacijske, memory-allocation i swapchain implementacije;
- compute je dovoljan za screen-space Radiance Cascades; taj pristup ne zahtijeva hardware ray tracing;
- SDL i dalje dopušta izlazak na direktni Vulkan ili Metal ako se kasnije pokaže nužno. ([SDL Vulkan glue](https://wiki.libsdl.org/SDL3/CategoryVulkan), [SDL Metal glue](https://wiki.libsdl.org/SDL3/CategoryMetal))

Ograničenja koja moraju ući u prototip:

- render pass ima najviše četiri color targeta i jedan depth target, dok trenutačni `R3D_TARGET_GBUFFER` sadrži šest color targeta; G-buffer se mora prepakirati ili podijeliti. ([SDL limit](https://wiki.libsdl.org/SDL3/CategoryGPU), [r3d target pack](../../src/modules/r3d_target.h))
- SDL_GPU nema 1D texture type; podržava 2D, 2D array, 3D, cube i cube array. Trenutačni skinning koristi 1D teksturu, pa je treba pretvoriti u 2D teksturu visine 1 ili storage buffer. ([SDL texture types](https://wiki.libsdl.org/SDL3/SDL_GPUTextureType), [r3d skin texture](../../include/r3d/r3d_skeleton.h))
- SDL_GPU format enum nema nekomprimirane trokanalne `RGB8`/`RGB16F` render targete, dok ih r3d koristi za više internih targeta. Port treba koristiti `RGBA8`, `RGBA16F` ili, gdje preciznost dopušta, `R11G11B10_UFLOAT`; dostupnost formata treba provjeravati službenim queryjem. ([SDL texture formats](https://wiki.libsdl.org/SDL3/SDL_GPUTextureFormat), [r3d formats](../../src/modules/r3d_target.c))
- shader resource layout je strogo definiran po backend formatu. SDL preporučuje reflection umjesto ručnog brojanja samplera, storage resursa i uniform buffera. ([graphics shader bindings](https://wiki.libsdl.org/SDL3/SDL_CreateGPUShader), [compute shader bindings](https://wiki.libsdl.org/SDL3/SDL_CreateGPUComputePipeline))
- SDL_GPU koristi left-handed konvenciju i depth `[0,1]`; SDL radi backend coordinate conversion, ali postojeće matrice, depth reconstruction, culling i screen-space shaderi svejedno trebaju golden-image testove. ([SDL_GPU coordinate system](https://wiki.libsdl.org/SDL3/CategoryGPU#coordinate-system))
- javni SDL_GPU API na datum istraživanja izlaže fence query, ali ne timestamp/pipeline-statistics query. Trenutačni r3d koristi `GL_TIME_ELAPSED`, pa GPU timing treba riješiti backend debug alatima ili zasebnim opcionalnim hookom. ([SDL_GPU API list](https://wiki.libsdl.org/SDL3/CategoryGPU#functions), [r3d timer](../../src/modules/r3d_driver.c))

SDL_GPU je mlad API: GPU tipovi postoje od SDL 3.2.0, a SDL 3.4 je već dodao dodatnu interoperabilnost s 2D rendererom. To opravdava mali feature-parity spike prije pune migracije, ali nije razlog za SDL2. ([shader format version](https://wiki.libsdl.org/SDL3/SDL_GPUShaderFormat), [SDL 3.4.0 release](https://github.com/libsdl-org/SDL/releases/tag/release-3.4.0))

### Shader build

SDL_GPU backendi ne koriste isti binarni shader format: Vulkan prima SPIR-V, Metal MSL ili `metallib`, a D3D12 DXBC/DXIL. SDL preporučuje offline prekompilaciju; službeni `SDL_shadercross` prima SPIR-V ili HLSL i daje DXBC, DXIL, SPIR-V, MSL ili HLSL, uz runtime i CLI varijante. ([SDL shader formats](https://wiki.libsdl.org/SDL3/SDL_GPUShaderFormat), [SDL_shadercross](https://github.com/libsdl-org/SDL_shadercross))

Za minimalan migracijski rizik:

1. zadržati postojeći GLSL kao privremeni source;
2. offline ga kompilirati u SPIR-V;
3. iz SPIR-V-a generirati MSL/`metallib` za Metal;
4. generirati reflection manifest i validirati SDL binding layout u buildu;
5. tek nakon vizualnog pariteta odlučiti isplati li se standardizirati buduće shadere na HLSL.

*Inference:* runtime shader cross-compilation ne treba biti production default. MoltenVK dokumentacija pokazuje da je shader conversion jedan od skupljih koraka te preporučuje offline/pipeline cache kako bi se izbjegli runtime zastoji; isti princip vrijedi i za drugi cross-compile put. ([MoltenVK shader loading](https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/Docs/MoltenVK_Runtime_UserGuide.md))

### Opcija B — vlastiti Vulkan + vlastiti Metal backend

SDL3 može ostati samo window/input sloj: Vulkan helperi daju potrebne instance ekstenzije i `VkSurfaceKHR`, a Metal helperi stvaraju `CAMetalLayer`-backed view; ostatak devicea, queueova, memoryja, synca, pipelineova i shadera ostaje aplikaciji. ([SDL Vulkan](https://wiki.libsdl.org/SDL3/CategoryVulkan), [SDL Metal view](https://wiki.libsdl.org/SDL3/SDL_Metal_CreateView))

Ovo daje punu kontrolu i pristup API-specifičnim naprednim mogućnostima, ali fork tada stvarno održava dva renderera. *Inference:* za mali projekt to nije održivo dok konkretan, izmjeren zahtjev ne probije SDL_GPU feature set. To nije prvi korak za kod koji danas nema ni Vulkan ni Metal backend.

### Opcija C — vlastiti Vulkan + MoltenVK na Apple platformama

MoltenVK je Khronosova Vulkan Portability implementacija koja mapira Vulkan 1.4 graphics/compute subset na Metal, automatski prevodi SPIR-V u MSL, koristi javne Apple API-je i kompatibilna je sa standardnom App Store distribucijom. Prednost je jedan Vulkan renderer na svim platformama. ([MoltenVK README](https://github.com/KhronosGroup/MoltenVK))

To nije besplatan native Metal backend:

- MoltenVK izričito nije potpuno Vulkan-conformant; razlike Metal/Vulkan ponekad zahtijevaju sintetizirano ponašanje ili ostaju nepodržane. ([MoltenVK compliance](https://github.com/KhronosGroup/MoltenVK#moltenvk-and-vulkan-compliance))
- device se mora tretirati kao `VK_KHR_portability_subset` i pri enumeraciji uključiti portability bit/ekstenziju. ([Vulkan portability enumeration](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_portability_enumeration.html), [MoltenVK setup](https://github.com/KhronosGroup/MoltenVK#using-the-vulkan-sdk))
- službeno navedena ograničenja uključuju nepodržane pipeline-statistics queryje i ignoriranje application allocation callbackova; Metal/extension zahtjevi razlikuju se po hardveru. ([MoltenVK limitations](https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/Docs/MoltenVK_Runtime_UserGuide.md))
- SPIR-V→MSL i Metal pipeline kompilacija traže cache strategiju da se izbjegnu startup i runtime hitching problemi. ([MoltenVK performance](https://raw.githubusercontent.com/KhronosGroup/MoltenVK/main/Docs/MoltenVK_Runtime_UserGuide.md))

**Zaključak:** MoltenVK ima smisla ako već postoji kvalitetan Vulkan renderer ili ako projekt treba Vulkan specifične mogućnosti koje SDL_GPU ne daje. Ovaj fork kreće iz OpenGL-a, pa nema postojeću Vulkan investiciju koju bi MoltenVK iskoristio. Uz SDL_GPU, MoltenVK bi bio suvišan jer SDL_GPU već ima native Metal backend.

### Ostale službeno potvrđene apstrakcije

- **bgfx** je graphics-API-agnostic „bring your own engine/framework” biblioteka s Vulkan i Metal backendima, C99 API-jem, vlastitim shader toolchainom i vlastitim render-thread/view modelom. Tehnički je valjana druga opcija ako SDL_GPU spike ne uspije. *Inference:* integracija bi bila invazivnija jer bgfx preuzima više render scheduling modela, dok SDL_GPU bliže odgovara minimalnom device/pipeline sloju koji ovom forku treba. ([bgfx README](https://github.com/bkaradzic/bgfx), [bgfx internals](https://bkaradzic.github.io/bgfx/internals.html))
- **Diligent Engine** podržava Vulkan, Metal i naprednije featuree, ali službeni README navodi da je native Metal backend dostupan pod komercijalnom licencom, dok Vulkan na Apple platformama traži MoltenVK ili sličan portability sloj. To je loš fit za otvoreni C fork koji želi native Metal bez dodatnog licenciranja. ([Diligent Engine](https://github.com/DiligentGraphics/DiligentEngine))

## 3. Radiance Cascades: 2D, „2.5D” ili 3D

### Tehnička osnova i razina dokaza

Izvorni Radiance Cascades rukopis autora Alexandera Sannikova definira polje kroz distance-bounded *radiance intervals*. Osnovna hipoteza je da blizu izvora/okludera treba visoka prostorna i niža kutna rezolucija, a daleko niža prostorna i viša kutna rezolucija. Kaskade zato povećavaju razmak proba i kutnu rezoluciju s udaljenošću, a susjedni intervali se kompozitiraju u puni radiance query. ([izvorni tekst](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))

Važna granica dokaza: autor navodi da rukopis nikad nije poslan u časopis i da je ostavljen kao trajno dostupan WIP. Peer-reviewed rad Osbornea i Sannikova potvrđuje matematičku/numeričku primjenu Radiance Cascades u multidimenzionalnom astrofizičkom radiative transferu, ali to nije benchmark real-time game renderera. ([status izvornog rukopisa](https://github.com/Raikiri/RadianceCascadesPaper), [peer-reviewed RASTI rad](https://academic.oup.com/rasti/article/doi/10.1093/rasti/rzae062/7929002))

### 2D / flatland

Flatland radiance field ima 2D poziciju i 1D kut. Izvorni rukopis pokazuje da memorija svake daljnje kaskade pada geometrijski i da je ukupan storage ograničen s približno dvostrukim storageom cascade 0. To je odlično za stvarne 2D igre, ali ne rješava samo po sebi depth, normale i skrivenost 3D scene. ([flatland analiza](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))

Holographic Radiance Cascades iz 2025. dodatno unapređuje 2D single-shot radiance transfer. Autori prijavljuju 1,85 ms na 512×512 i 7,67 ms na 1024×1024 na RTX 3080 Laptop GPU-u. To je još uvijek arXiv preprint i eksplicitno 2D rezultat; nije dokaz za 3D/isometric produkcijski pipeline. ([HRC preprint](https://arxiv.org/abs/2505.02041))

### Screen-space 3D, ovdje nazvan „2.5D”: preporuka

Postoji autoritativan javni dokaz za Path of Exile 2:

- GGG službeno opisuje ExileCon 2023 predavanje Sannikova kao prezentaciju tehnika korištenih u renderiranju Path of Exile 2. ([GGG](https://www.pathofexile.com/forum/view-thread/3447463/filter-account-type/staff))
- Sannikovljev izvorni rukopis izričito kaže da je Radiance Cascades integriran u PoE2 kao GI rješenje te da koristi flatland-like screen-space hierarchy i screen-space raymarching, motivirano fiksnom kamerom. ([rukopis](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))
- Autor prijavljuje oko 3 ms na GTX 1050 i opisuje optimizaciju koja istodobno raymarchira četiri zrake te sprema okluziju u 128-bitni bitmask. To je first-party broj, ali nije reproducibilan benchmark za ovaj fork. ([PoE2 implementacija u rukopisu](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))
- Isti izvor jasno navodi granice: off-screen izvori ne daju indirect light, nevidljivi okluderi ne bacaju sjenu i nema geometrije iza depth buffera. ([ograničenja](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))

To je praktično „2.5D”: scene i shading su 3D, ali probe/cascade raspored i visibility potječu iz projekcije/G-buffera. Za igru s kontroliranom izometrijskom perspektivom to je najbolji početni omjer cijene i kvalitete.

Prvi milestone treba biti:

- jedna dinamička diffuse indirect bounce komponenta iz emissive i direktno osvijetljenih screen-space površina;
- depth/normal-aware probe interpolation;
- potpuna rekonstrukcija svake slike bez obveznog temporal accumulationa;
- debug prikazi za svaku kaskadu, visibility intervale i finalni indirect contribution;
- izmjerena usporedba protiv postojećeg SSGI-ja i scene bez GI-ja.

Specular convolution je moguć prema izvornom radu, ali nije prvi milestone; prvo treba potvrditi stabilan diffuse rezultat. ([diffuse/specular uporaba](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))

### Puni 3D / world-space

Puni 3D RC koristi 3D grid proba i 2D kutnu distribuciju. Prednost je query bilo gdje u volumenu, off-screen rasvjeta i podrška za participating media. Problem je cascade 0: njezin storage raste s volumenom scene na treću potenciju i autor ga navodi kao mogući deal-breaker. ([3D analiza i ograničenja](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))

Hibrid „screen-space probes with world-space intervals” može uhvatiti off-screen emitere i okludere pomoću SDF-a, BVH-a, software ili hardware ray tracinga. U autorovu eksperimentu bio je jedan do dva reda veličine sporiji od PoE2 screen-space varijante i oslanjao se na temporal convergence. To je istraživački fallback ako screen-space artefakti postanu neprihvatljivi, ne početna implementacija. ([world-space intervals](https://raw.githubusercontent.com/Raikiri/RadianceCascadesPaper/main/RadianceCascades.tex))

### Što nije javno poznato

Javno je autoritativno potvrđeno da PoE2 koristi opisanu screen-space RC strukturu. **Nije javno objavljen kompletan produkcijski source, svi parametri, pass layout, fallbackovi, platform-specifične varijante ni sadašnji trošak na shipping buildu.** Sam autor kaže da ima još mnogo detalja, ali da su izvan opsega rukopisa. Stoga se PoE2 ne može kopirati jedan-za-jedan; može se reproducirati samo dokumentirana jezgra i validirati na scenama ove igre. ([rukopis i njegov opseg](https://github.com/Raikiri/RadianceCascadesPaper))

## 4. Predloženi arhitektonski rez

```text
game
  └─ r3d public API (vlastiti POD tipovi i opaque handleovi)
       ├─ scene / materials / visibility
       ├─ frame graph i renderer passes
       │    └─ optional radiance-cascades pass
       ├─ GPU abstraction (minimalni interni contract)
       │    └─ SDL_GPU backend
       └─ platform abstraction
            └─ SDL3 backend
```

GPU contract treba projektirati iz potrebnih passova igre, ne kao presliku cijelog Vulkan/Metal API-ja. SDL_GPU tipovi ne smiju curiti u javni r3d API; tako SDL_GPU može biti zamijenjen bez još jednog public-API reza. *Inference:* frame graph je koristan zato što Vulkan/Metal zahtijevaju eksplicitnije pass/resource lifetime granice od postojećeg OpenGL state cachea, a Radiance Cascades dodaje više compute i merge passova.

## 5. Obvezni spike prije pune migracije

Jedan vertikalni prototip treba dokazati, na Vulkanu i Metalu:

1. SDL3 window/input lifecycle i resize/high-DPI;
2. shader offline build za SPIR-V i `metallib`, uz reflection manifest;
3. repakirani G-buffer s najviše četiri MRT-a;
4. skeletal skinning bez 1D teksture;
5. cube-array shadow map i minimalni PBR opaque pass;
6. jedan transparent/forward pass;
7. jedan compute pass koji piše i zatim samplea `RGBA16F` storage teksturu;
8. reprezentativni compute interval-trace kernel koji dokazuje storage, binding i dispatch potrebe budućeg screen-space RC-a, bez production RC integracije prije rendererskog pariteta;
9. golden screenshots i GPU capture na oba backenda.

Stop uvjeti za SDL_GPU su konkretni: nemoguće reproducirati obvezan game feature, neprihvatljiva izmjerena regresija koju potvrđuje GPU capture, ili potreba za nepodržanim API featureom. Tek tada usporediti bgfx s vlastitim Vulkan+Metal ili Vulkan+MoltenVK putem, koristeći isti vertikalni test.

## Konačna preporuka

**SDL3 + SDL_GPU, native Metal na macOS-u i Vulkan na Windowsu/Linuxu, s vlastitim uskim r3d API-jem iznad njih.** MoltenVK nije prvi izbor jer nema postojećeg Vulkan backenda koji bi pojednostavio; vlastita dva backenda su nepotreban maintenance multiplier; Diligentov native Metal ima licencni problem; bgfx ostaje razuman plan B.

Za GI, **ne graditi čistu 2D niti punu 3D Radiance Cascades varijantu prvo**. Početi screen-space „2.5D” pristupom dokumentiranim za PoE2, prihvatiti njegove jasne screen-space granice i tek nakon profiliranja odlučiti treba li mali world-space/off-screen dodatak.
