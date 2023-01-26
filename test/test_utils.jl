# These are tools that help perform the LaMEM tests, which run LaMEM locally

"""
    run_lamem_local_test(ParamFile::String, cores::Int64=1, args::String=""; 
                        outfile="test.out", bin_dir="../../bin", opt=true, deb=false,
                        mpiexec="mpiexec")

This runs a LaMEM simulation with given `ParamFile` on 1 or more cores, while writing the output to a local log file.

"""
function run_lamem_local_test(ParamFile::String, cores::Int64=1, args::String=""; 
                outfile="test.out", bin_dir="../../bin", opt=true, deb=false,
                mpiexec="mpiexec")
    
    cur_dir = pwd()
    if opt
        exec=joinpath(cur_dir,bin_dir,"opt","LaMEM")
    elseif deb
        exec=joinpath(cur_dir,bin_dir,"deb","LaMEM")
    end

    success = true
    if cores==1
        perform_run = `$(exec) -ParamFile $(ParamFile) $args`;
        
        # Run LaMEM on a single core, which does not require a working MPI
        try 
            if !isempty(outfile)
                run(pipeline(perform_run, stdout=outfile));
            else
                run(perform_run);
            end
        catch
            println("An error occured in directory: $(cur_dir) ")
            println("while running the script:")
            println(perform_run)
            success = false
        end
    else
        perform_run = `$(mpiexec) -n $(cores) $(exec) -ParamFile $(ParamFile) $args`;
        # set correct environment
        #mpirun = setenv(mpiexec, LaMEM_jll.JLLWrappers.JLLWrappers.LIBPATH_env=>LaMEM_jll.LIBPATH[]);
        # Run LaMEM in parallel
        try 
            if !isempty(outfile)
                run(pipeline(perform_run, stdout=outfile));
            else
                run(perform_run);
            end
        catch
            println(perform_run)
            success = false
        end
    end
  

    return success
end


"""
    out = extract_info_logfiles(file::String, keywords::NTuple{N,String}=("|Div|_inf","|Div|_2","|mRes|_2"), split_sign="=")

Extracts values from the logfile `file` specified after `keywords` (optionally defining a `split_sign`).
This will generally return a NamedTuple with Vectors 

"""
function extract_info_logfiles(file::String, keywords::NTuple{N,String}=("|Div|_inf","|Div|_2","|mRes|_2"), split_sign="=") where N

    out=()
    for i=1:N        
        d =  []
        open(file) do f
            while ! eof(f) 
                # read a new / next line for every iteration          
                line = readline(f)
                    if contains(line, keywords[i])
                        num = parse(Float64,split(line,split_sign)[end])
                        push!(d,num)    # add value to vector
                    end
            end
        end
        
        out =  (out..., Float64.(d))    # add vector to tuple
    end

    out_NT = NamedTuple{Symbol.(keywords)}(out)

    return out_NT
end


"""
    success = compare_logfiles(new::String, expected::String, 
                        keywords::NTuple{N,String}=("|Div|_inf","|Div|_2","|mRes|_2"), 
                        accuracy=((rtol=1e-6,), (rtol=1e-6,), (rtol=1e-6,)))

This compares two logfiles (different parameters which can be indicated). If the length of the vectors is not the same, or the accuracy criteria are not met, success=false and info is displayed, to help track down the matter.

"""
function compare_logfiles(new::String, expected::String, 
                        keywords::NTuple{N,String}=("|Div|_inf","|Div|_2","|mRes|_2"), 
                        accuracy=((rtol=1e-6,), (rtol=1e-6,), (rtol=1e-6,))) where N

    new_out = extract_info_logfiles(new, keywords)
    exp_out = extract_info_logfiles(expected, keywords)

    test_status = true
    for i=1:N 
        rtol, atol = 0,0
        if  haskey(accuracy[i], :rtol)
            rtol = accuracy[i].rtol;
        end
        if  haskey(accuracy[i], :atol)
            atol = accuracy[i].atol;
        end
        if length(new_out[i])==length(exp_out[i])
            te =  isapprox(new_out[i], exp_out[i], rtol=rtol, atol=atol)
            if te==false
                println("Problem with comparison of $(keywords[i]):")
                print_differences( new_out[i], exp_out[i], accuracy[i])
                test_status = false
            end
        else
            println("Problem with comparison of $(keywords[i]):")
            println("length of vectors not the same")
            test_status = false
        end
       
    end
  

    return test_status
end

# Pretty formatting of errors
function print_differences(new, expected, accuracy)
    n = 24;
    println("      $(rpad("New",n)) | $(rpad("Expected",n)) | $(rpad("rtol",n)) | $(rpad("atol",n))")


    for i=1:length(new)
        atol = abs(new[i] - expected[i])
        rtol = abs(atol/new[i])
        println("$(rpad(i,4))  $(rpad(new[i],n)) | $(rpad(expected[i],n)) | $(rpad(rtol,n)) | $(rpad(atol,n))")
    end

    return nothing
end


function perform_lamem_test(dir::String, ParamFile::String, expectedFile::String; 
                keywords=("|Div|_inf","|Div|_2","|mRes|_2"), accuracy=((rtol=1e-6,), (rtol=1e-6,), (rtol=1e-6,)), 
                cores::Int64=1, args::String="",
                bin_dir="../bin",  opt=true, deb=false, mpiexec="mpiexec",
                debug=false)

    cur_dir = pwd();
    cd(dir)

    bin_dir = joinpath(cur_dir,bin_dir);
    if debug==true
        outfile = "";
    else
        outfile = "test_$(cores).out";
    end

    # perform simulation 
    success = run_lamem_local_test(ParamFile, cores, args, outfile=outfile, bin_dir=bin_dir, opt=opt, deb=deb, mpiexec=mpiexec);

    if success
        # compare logfiles 
        success = compare_logfiles(outfile, expectedFile, keywords, accuracy)
    end

    if !success
        # something went wrong with executing the file (likely @ PETSc error)
        # Display some useful info here that helps debugging
        println("Problem detected with test; see this on commandline with: ")
        println("  dir=$(cur_dir) ")
        println("  ParamFile=$(ParamFile) ")
        println("  cores=$(cores) ")
        println("  args=$(args) ")
        println("  outfile=$(outfile) ")
        println("  bindir=$(bin_dir) ")
        println("  opt=$(opt) ")
        println("  deb=$(deb) ")
        println("  mpiexec=$(mpiexec) ")
        println("  success = run_lamem_local_test(ParamFile, cores, args, outfile=nothing, bin_dir=bin_dir, opt=opt, deb=deb, mpiexec=mpiexec);")
    end

    cd(cur_dir)  # return to directory       
    
    return success
end 

