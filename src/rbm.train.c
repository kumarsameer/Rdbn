/*
 * rbm.train.c -- Trains a restricted Boltzman machine using contrastive divergence.
 * 
 * Supports parallel processing using pthreads (if availiable).
 *
 */
#include <R.h>
#include <Rdefines.h>
#include <Rmath.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>
#include <R_ext/Applic.h>
#include <pthread.h>
#include "rbm.h"
#include "rbm.train.h"
#include "matrix_functions.h"

/**********************************************************************
 Functions for getting/ updating during training. */
 
/*Subtract each element of recon from data.  The result will be passed back in data.*/
void compute_delta_w(rbm_t *rbm, delta_w_t *batch, double *init_output_recon, double *input_example, double *output_recon, double *input_recon) {
  for(int i=0;i<rbm[0].n_outputs;i++) {
    batch[0].delta_output_bias[i]+= init_output_recon[i]-output_recon[i];
    for(int j=0;j<rbm[0].n_inputs;j++) {
      double delta_w_i_j= get_matrix_value(batch[0].delta_w, i, j)+
			(sample_state(init_output_recon[i])*input_example[j])-(output_recon[i]*input_recon[j]); // <ViHj_data>-<ViHj_recon>
      set_matrix_value(batch[0].delta_w, i, j, delta_w_i_j); // Really need to inline these setter-getter functions.
	  
      if(i==0) // Only 
        batch[0].delta_input_bias[j]+= input_example[j]-input_recon[j]; 
    }
  }
}

/*
 * Add matricies io_weights and delta_w.  The result will be in io_weights.
 *
 * Also includes 
 */
void apply_delta_w(rbm_t *rbm, delta_w_t *dw) {
  for(int i=0;i<rbm[0].n_outputs;i++) {
    rbm[0].bias_outputs[i] += dw[0].learning_rate*dw[0].delta_output_bias[i]/(double)dw[0].batch_size; 
    for(int j=0;j<rbm[0].n_inputs;j++) {
      double previous_w_i_j= get_matrix_value(rbm[0].io_weights, i, j);
      double delta_w_i_j= get_matrix_value(dw[0].delta_w, i, j);
      double new_w_i_j= previous_w_i_j+dw[0].learning_rate*delta_w_i_j/(double)dw[0].batch_size;
      set_matrix_value(rbm[0].io_weights, i, j, new_w_i_j);
	  
      if(i==0 && dw[0].update_input_bias) // Only update once... and if everything says to update.
        rbm[0].bias_inputs[j] += dw[0].learning_rate*dw[0].delta_input_bias[j]/(double)dw[0].batch_size;
    }
  }
}

/*
 * These functions for updating both weights and velocities together, using the momentum approach.
 *
 *  We use the momentum approach suggested by Nesterov, and adapted by Ilya Sutskever.
 *  Described here: http://www.cs.utoronto.ca/~ilya/pubs/ilya_sutskever_phd_thesis.pdf.
 *  See pp. 75 of the Sutskever thesis, and in particular equations 7.10-7.11.
 *
 *  Currently does NOT use momentum approach for updating biases, although that is straightforward to implement.
 */

void initial_momentum_step(rbm_t *rbm) {
  for(int i=0;i<rbm[0].n_outputs;i++)
    for(int j=0;j<rbm[0].n_inputs;j++) {
      // Computes(eq 7.11, 1st half): v_t=\mu_{t-1}v_{t-1}.
      double momentum_i_j= rbm[0].momentum_decay*get_matrix_value(rbm[0].momentum, i, j); 
      set_matrix_value(rbm[0].momentum, i, j, momentum_i_j); // Updates momentum matrix.

      // Computes(eq 7.10, 1st half): \theta_t = \theta_{t-1} + \mu_{t-11}v_{t-1}.
      set_matrix_value(rbm[0].io_weights, i, j, get_matrix_value(rbm[0].io_weights, i, j)+momentum_i_j);
    }
}
 
void apply_momentum_correction(rbm_t *rbm, delta_w_t *dw) {
  for(int i=0;i<rbm[0].n_outputs;i++) {
    rbm[0].bias_outputs[i]+= dw[0].learning_rate*dw[0].delta_output_bias[i]/(double)dw[0].batch_size; 
    for(int j=0;j<rbm[0].n_inputs;j++) {
      double step= dw[0].learning_rate*get_matrix_value(dw[0].delta_w, i, j)/(double)dw[0].batch_size; // For the momentum method ... do I still scale by the batch size?!

      // Update weights.  \theta_t = \theta_t' - \epsilon_{t-1} \gradient_f(\theta_{t-1} + \mu_{t-1}v_{t-1}) // (eq. 7.10, 2nd half).
      // \theta_t' was applied before taking the step.
      double previous_w_i_j= get_matrix_value(rbm[0].io_weights, i, j);
      set_matrix_value(rbm[0].io_weights, i, j, previous_w_i_j+step);  //  
	  
      // Update velocities.  v_t = v_t' - \epsilon_{t-1} \gradient_f(\theta_{t-1} + \mu_{t-1}v_{t-1}) // (eq. 7.11, 2nd half).
      // v_t' was applied before taking the step.
      double previous_momentum_i_j= get_matrix_value(rbm[0].momentum, i, j);
        set_matrix_value(rbm[0].momentum, i, j, previous_momentum_i_j+step);

      if(i==0 && dw[0].update_input_bias) // Only update once... and if everything says to update.
        rbm[0].bias_inputs[j]+= dw[0].learning_rate*dw[0].delta_input_bias[j]/(double)dw[0].batch_size;
    }
  }
}


/*  
 * Processes a single batch element, and increments the delta_w, delta_bias_input, and delta_bias_output
 *  weights accordingly.
 *
 * Arguments:
 *    rbm --> Information on the restricted boltzman machine.
 *    
 *
 */
void do_batch_member(rbm_t *rbm,  double *input_example, delta_w_t *batch) {
 
  // Run Gibbs sampling for CDn steps.
  double *init_output_recon= (double*)Calloc(rbm[0].n_outputs, double);
  double *input_recon= (double*)Calloc(rbm[0].n_inputs, double);
  clamp_input(rbm, input_example, init_output_recon); // Compute p(hj=1 | v)= logistic_sigmoid(b_j+\sum(v_i * w_ij))
  double *output_recon= vector_copy(init_output_recon, rbm[0].n_outputs);  
  for(int cd=0;cd<rbm[0].cd_n;cd++) {
    clamp_output(rbm, output_recon, input_recon); // Get the input_recon(struction), using the output from the previous step.
    clamp_input(rbm, input_recon, output_recon); // Get the output_recon(struction), using the input from the previous step.
  }
     
  // Put all of this together so that only 1 loop is required ...
  // Compute  ... in this computation, sample random states (?!).
  // Compute <vihj>_data-<vihj>_recon and update batch.
  compute_delta_w(rbm, batch, init_output_recon, input_example, output_recon, input_recon);

  Free(init_output_recon); Free(output_recon); Free(input_recon);
}

/************************************************************************************
 *
 *  Optimize the weights weights over a training dataset using contrastive divergence.
 *
 *  Arguments:
 *    rbm           --> Restricted boltzman machine.
 *    input_example --> A matrix (n_inptus x batch_size) of input examples.  
 *
 *  Following recommendations in: http://www.cs.toronto.edu/~hinton/absps/guideTR.pdf (V. 1; dated: Aug. 2nd 2010).
 *
 * Code currently trains by taking the MEAN of gradients in a batch!  This seems most intiutive to me, and more
 *  or less agrees with comments in: http://www.elen.ucl.ac.be/Proceedings/esann/esannpdf/es2012-71.pdf.
 * Don't know what all these how-to pages are talking about when they say 'multiplying matrices', though!?
 */
void *rbm_partial_minibatch(void *ptab) {
  rbm_pthread_arg_t *pta= (rbm_pthread_arg_t*)ptab;
  rbm_t *rbm= pta[0].rbm;
  double *input_example= pta[0].input;
    
  // Compute the \sum gradient over the mini-batch.
  for(int i=0;i<pta[0].do_n_elements;i++) { // Foreach item in the batch.
    do_batch_member(rbm, input_example, pta[0].batch);
    input_example+= rbm[0].n_inputs; // Update the input_example pointer to the next input sample.
  }
  
}

void do_minibatch_pthreads(rbm_t *rbm, double *input_example, int n_threads) { // Use velocity?!; Use sparsity target?!  // Change name to 
  // If using momentum Take a step BEFORE computing the local gradient.
  if(rbm[0].use_momentum) { 
    initial_momentum_step(rbm);
  }
  
  // Activate each as a separate thread.
  rbm_pthread_arg_t *pta= (rbm_pthread_arg_t*)Calloc(n_threads, rbm_pthread_arg_t);
  pthread_t *threads= (pthread_t*)Calloc(n_threads, pthread_t);
  int n_per_batch= floor(rbm[0].batch_size/n_threads);
  int remainder= (rbm[0].batch_size%n_threads==0)?n_per_batch:(rbm[0].batch_size%n_threads);
  for(int i=0;i<n_threads;i++) {
    // Set up data passed to partial_minibatch()
    pta[i].rbm= rbm;
    pta[i].input= input_example;
    pta[i].do_n_elements= (i<(n_threads-1))?n_per_batch:remainder; // For the last thread, only run remaining elements.
    pta[i].batch= alloc_dwt_from_rbm(rbm);
    pthread_create(threads+i, NULL, rbm_partial_minibatch, (void*)(pta+i));
	
    // Increment pointers for the next thread.
    input_example+= pta[i].do_n_elements*rbm[0].n_inputs;
  }

  // Wait for threads to complete, and combine the data into a single vector.
  delta_w_t *batch;
  for(int i=0;i<n_threads;i++) {
    pthread_join(threads[i], NULL); // Wait for thread to finish.
	
    if(i==0) {
      batch= pta[i].batch;
    }
    else {
      sum_delta_w(batch, pta[i].batch);
      free_delta_w_ptr(pta[i].batch, 1);
    }
  }
  Free(pta); Free(threads);
    
  // Take a step in teh direction of the gradient.
  if(rbm[0].use_momentum) { // Correct and update momentum term.
    apply_momentum_correction(rbm, batch); 
  }
  else { // Update weights. \delta w_{ij} = \epislon * (<v_i h_j>_data - <v_i h_j>recon 
    apply_delta_w(rbm, batch);
  }
  
  // Cleanup temporary variables ...  
  free_delta_w_ptr(batch, 1); 
}

 
void do_minibatch(rbm_t *rbm, double *input_example, int n_threads) { // Use velocity?!; Use sparsity target?!  // Change name to 
  // If using momentum Take a step BEFORE computing the local gradient.
  if(rbm[0].use_momentum) { 
    initial_momentum_step(rbm);
  }

  rbm_pthread_arg_t pta;
  pta.rbm= rbm;
  pta.input= input_example;
  pta.do_n_elements= rbm[0].batch_size;
  pta.batch= alloc_dwt_from_rbm(rbm);
  rbm_partial_minibatch(&pta);
  
  // Take a step in teh direction of the gradient.
  if(rbm[0].use_momentum) { // Correct and update momentum term.
    apply_momentum_correction(rbm, pta.batch); 
  }
  else { // Update weights. \delta w_{ij} = \epislon * (<v_i h_j>_data - <v_i h_j>recon 
    apply_delta_w(rbm, pta.batch);
  }
  
  // Cleanup temporary variables ...  
  free_delta_w_ptr(pta.batch, 1); 
}

/*
 * Shortcut to loop over examples for a given number of epocs.
 *
 * Arguments: 
 *   rbm           --> Restricted boltzman machine.
 *   input_example --> Input example used for training.  Each columns represents a unique training case.
 *   n_examples    --> Specifies the number of examples provided.
 *   n_epocs       --> Specifies the number of times the training program loops over the examples.
 *
 * Assumptions: 
 *   --> n_examples is a multiple of rbm.batch_size ... additional examples are ignored.
 */
void rbm_train(rbm_t *rbm, double *input_example, int n_examples, int n_epocs, int n_threads) {
  double *current_position;
  int n_training_iterations= floor(n_examples/rbm[0].batch_size); 

  for(int i=0;i<n_epocs;i++) {
    //if(i%(100)) Rprintf(".");
    current_position= input_example; // Reset training pointer.
    for(int j=0;j<n_training_iterations;j++) {
      if(n_threads >0)
        do_minibatch_pthreads(rbm, current_position, n_threads);  // Do a minibatch using the current position of the training pointer.
      else 
        do_minibatch(rbm, current_position, n_threads);
      current_position+= rbm[0].batch_size*rbm[0].n_inputs; // Increment the input_example pointer batch_size # of columns.
	}
  }

}



/************************************************************************************
 *
 *  An R interface for RBM training ...
 */

SEXP train_rbm_R(SEXP rbm_r, SEXP training_data_r, SEXP n_epocs_r, SEXP n_threads_r) {
  rbm_t *rbm= rbm_r_to_c(rbm_r); // Get values from R function.

  int n_examples= Rf_nrows(training_data_r)/rbm[0].n_inputs;
  int n_epocs= INTEGER(n_epocs_r)[0];
  int n_threads= INTEGER(n_threads_r)[0];
  double *input_example= REAL(training_data_r);

  rbm_train(rbm, input_example, n_examples, n_epocs, n_threads);
  
  return(rbm_r);
}